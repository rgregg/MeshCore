#!/usr/bin/env python3
"""
MeshCore EthernetBridge packet monitor.

Listens on UDP for packets forwarded by EthernetBridge and decodes them
into human-readable output.

Usage:
    python3 bridge_monitor.py [--port 5005] [--bind 0.0.0.0] [--hex]
                              [--channel NAME:SECRET_HEX ...]

    # Decrypt a specific group channel (secret = 64 hex chars = 32 bytes):
    python3 bridge_monitor.py --channel "Default:aabbcc..."

    # Multiple channels:
    python3 bridge_monitor.py --channel "Default:aabb..." --channel "ops:ccdd..."

Decryption requires pycryptodome:
    pip install pycryptodome

Wire format (EthernetBridge UDP datagram):
    [2]  Magic      0xC03E
    [2]  Length     big-endian, byte count of mesh packet
    [n]  Payload    serialized mesh::Packet
    [2]  Checksum   Fletcher-16 over payload

Group channel encryption (Utils::encryptThenMAC):
    AES128-ECB with secret[0:16] as key
    HMAC-SHA256 over ciphertext using secret[0:32], truncated to 2 bytes
    Wire: [MAC:2][ciphertext:n*16]

GRP_TXT plaintext layout:
    [timestamp:4 LE][txt_type:1][sender_name: message text (null-padded)]
"""

import socket
import struct
import argparse
import datetime
import sys
import hmac
import hashlib

# Optional AES for channel decryption
try:
    from Crypto.Cipher import AES as _AES
    AES_AVAILABLE = True
except ImportError:
    AES_AVAILABLE = False

# ── Constants (mirrors MeshCore.h / Packet.h) ────────────────────────────────

BRIDGE_MAGIC         = 0xC03E
BRIDGE_OVERHEAD      = 6         # magic(2) + length(2) + checksum(2)

PUB_KEY_SIZE         = 32
SIGNATURE_SIZE       = 64
MAX_ADVERT_DATA_SIZE = 32
CIPHER_KEY_SIZE      = 16        # AES128
CIPHER_MAC_SIZE      = 2         # HMAC prefix bytes

ROUTE_TYPE = {
    0x00: "TRANSPORT_FLOOD",
    0x01: "FLOOD",
    0x02: "DIRECT",
    0x03: "TRANSPORT_DIRECT",
}

PAYLOAD_TYPE = {
    0x00: "REQ",
    0x01: "RESPONSE",
    0x02: "TXT_MSG",
    0x03: "ACK",
    0x04: "ADVERT",
    0x05: "GRP_TXT",
    0x06: "GRP_DATA",
    0x07: "ANON_REQ",
    0x08: "PATH",
    0x09: "TRACE",
    0x0A: "MULTIPART",
    0x0B: "CONTROL",
    0x0F: "RAW_CUSTOM",
}

ADV_NODE_TYPE = {
    0: "unknown",
    1: "chat",
    2: "repeater",
    3: "room",
    4: "sensor",
}

ADV_LATLON_MASK = 0x10
ADV_FEAT1_MASK  = 0x20
ADV_FEAT2_MASK  = 0x40
ADV_NAME_MASK   = 0x80

# ANSI colours
RESET   = "\033[0m"
BOLD    = "\033[1m"
CYAN    = "\033[36m"
GREEN   = "\033[32m"
YELLOW  = "\033[33m"
MAGENTA = "\033[35m"
RED     = "\033[31m"
DIM     = "\033[2m"


# ── Fletcher-16 checksum ──────────────────────────────────────────────────────

def fletcher16(data: bytes) -> int:
    s1, s2 = 0, 0
    for b in data:
        s1 = (s1 + b) % 255
        s2 = (s2 + s1) % 255
    return (s2 << 8) | s1


# ── Channel decryption (mirrors Utils::MACThenDecrypt) ────────────────────────

def mac_then_decrypt(secret: bytes, src: bytes) -> bytes | None:
    """
    Verify HMAC-SHA256[0:2] then AES128-ECB decrypt.
    Returns plaintext bytes or None if MAC fails or AES unavailable.
    """
    if not AES_AVAILABLE or len(src) <= CIPHER_MAC_SIZE:
        return None

    mac_recv  = src[:CIPHER_MAC_SIZE]
    ciphertext = src[CIPHER_MAC_SIZE:]

    mac_calc = hmac.new(secret, ciphertext, hashlib.sha256).digest()[:CIPHER_MAC_SIZE]
    if mac_calc != mac_recv:
        return None

    cipher = _AES.new(secret[:CIPHER_KEY_SIZE], _AES.MODE_ECB)
    return cipher.decrypt(ciphertext)


def try_decrypt_group(payload: bytes, channels: dict) -> tuple[str | None, bytes | None]:
    """
    Try all known channel secrets against a GRP_TXT/GRP_DATA payload.
    Returns (channel_name, plaintext) or (None, None).
    Payload layout: [channel_hash:1][mac:2][ciphertext...]
    """
    if len(payload) < 3:
        return None, None

    mac_and_ct = payload[1:]  # skip channel_hash byte; MAC starts immediately
    for name, secret in channels.items():
        plaintext = mac_then_decrypt(secret, mac_and_ct)
        if plaintext is not None:
            return name, plaintext
    return None, None


def parse_grp_txt_plaintext(plaintext: bytes) -> dict:
    """
    GRP_TXT decrypted layout:
      [timestamp:4 LE uint32][txt_type:1]["sender: message\0" null-padded]
    """
    if len(plaintext) < 5:
        return {"error": "plaintext too short"}

    timestamp = struct.unpack_from("<I", plaintext, 0)[0]
    txt_type  = plaintext[4]
    text_raw  = plaintext[5:].rstrip(b"\x00")
    text      = text_raw.decode("utf-8", errors="replace")

    return {
        "timestamp": timestamp,
        "time_utc":  datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).strftime("%H:%M:%S UTC") if timestamp else "?",
        "txt_type":  txt_type,
        "text":      text,
    }


# ── Frame parser ──────────────────────────────────────────────────────────────

def parse_frame(data: bytes) -> bytes | None:
    """Validate EthernetBridge framing and return inner mesh packet bytes."""
    if len(data) < BRIDGE_OVERHEAD:
        print(f"{RED}Frame too short ({len(data)} bytes){RESET}")
        return None

    magic = struct.unpack_from(">H", data, 0)[0]
    if magic != BRIDGE_MAGIC:
        print(f"{RED}Bad magic: 0x{magic:04X}{RESET}")
        return None

    length = struct.unpack_from(">H", data, 2)[0]
    if len(data) < length + BRIDGE_OVERHEAD:
        print(f"{RED}Truncated frame: expected {length + BRIDGE_OVERHEAD}, got {len(data)}{RESET}")
        return None

    payload       = data[4 : 4 + length]
    recv_checksum = struct.unpack_from(">H", data, 4 + length)[0]
    calc_checksum = fletcher16(payload)

    if recv_checksum != calc_checksum:
        print(f"{RED}Checksum mismatch: got 0x{recv_checksum:04X}, expected 0x{calc_checksum:04X}{RESET}")
        return None

    return payload


# ── Mesh packet decoder ───────────────────────────────────────────────────────

def decode_packet(raw: bytes) -> dict | None:
    """Decode a serialized mesh::Packet (writeTo format)."""
    if len(raw) < 2:
        return None

    i = 0
    header = raw[i]; i += 1

    route_type   = header & 0x03
    payload_type = (header >> 2) & 0x0F
    payload_ver  = (header >> 6) & 0x03

    has_transport   = route_type in (0x00, 0x03)
    transport_codes = None
    if has_transport:
        if len(raw) < i + 4:
            return None
        tc0 = struct.unpack_from("<H", raw, i)[0]; i += 2
        tc1 = struct.unpack_from("<H", raw, i)[0]; i += 2
        transport_codes = (tc0, tc1)

    if i >= len(raw):
        return None
    path_len_byte = raw[i]; i += 1

    hash_count = path_len_byte & 0x3F
    hash_size  = (path_len_byte >> 6) + 1

    path = []
    for _ in range(hash_count):
        path.append(raw[i : i + hash_size].hex())
        i += hash_size

    payload = raw[i:]

    return {
        "route_type":      route_type,
        "payload_type":    payload_type,
        "payload_ver":     payload_ver,
        "transport_codes": transport_codes,
        "path":            path,
        "hash_size":       hash_size,
        "payload":         payload,
    }


# ── Payload decoders ──────────────────────────────────────────────────────────

def decode_advert(payload: bytes) -> dict:
    i = 0
    if len(payload) < PUB_KEY_SIZE + 4 + SIGNATURE_SIZE:
        return {"error": "too short"}

    pub_key   = payload[i : i + PUB_KEY_SIZE]; i += PUB_KEY_SIZE
    timestamp = struct.unpack_from("<I", payload, i)[0]; i += 4
    # skip signature (64 bytes)
    i += SIGNATURE_SIZE

    result = {
        "pub_key":  pub_key.hex(),
        "node_id":  pub_key[:4].hex(),
        "time_utc": datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC") if timestamp else "unset",
    }

    app_data = payload[i:]
    if len(app_data) >= 1:
        flags     = app_data[0]
        node_type = flags & 0x0F
        result["node_type"] = ADV_NODE_TYPE.get(node_type, f"0x{node_type:02X}")
        j = 1

        if flags & ADV_LATLON_MASK and len(app_data) >= j + 8:
            lat = struct.unpack_from("<i", app_data, j)[0] / 1_000_000; j += 4
            lon = struct.unpack_from("<i", app_data, j)[0] / 1_000_000; j += 4
            result["lat"] = lat
            result["lon"] = lon

        if flags & ADV_FEAT1_MASK and len(app_data) >= j + 2:
            result["feat1"] = struct.unpack_from("<H", app_data, j)[0]; j += 2

        if flags & ADV_FEAT2_MASK and len(app_data) >= j + 2:
            result["feat2"] = struct.unpack_from("<H", app_data, j)[0]; j += 2

        if flags & ADV_NAME_MASK and j < len(app_data):
            result["name"] = app_data[j:].decode("utf-8", errors="replace").rstrip("\x00")

    return result


def decode_encrypted_header(payload: bytes, label: str) -> dict:
    """REQ/RESPONSE/TXT_MSG/PATH: [dest_hash:1][src_hash:1][mac:2][encrypted...]"""
    if len(payload) < 4:
        return {"error": "too short"}
    return {
        "dest_hash": payload[0:1].hex(),
        "src_hash":  payload[1:2].hex(),
        "mac":       payload[2:4].hex(),
        "enc_len":   len(payload) - 4,
        "note":      f"payload encrypted ({label})",
    }


def decode_anon_req(payload: bytes) -> dict:
    """ANON_REQ: [dest_hash:1][ephemeral_pub_key:32][mac:2][encrypted...]"""
    if len(payload) < 35:
        return {"error": "too short"}
    return {
        "dest_hash":     payload[0:1].hex(),
        "ephemeral_key": payload[1:33].hex(),
        "mac":           payload[33:35].hex(),
        "enc_len":       len(payload) - 35,
        "note":          "payload encrypted (ANON_REQ)",
    }


# ── Pretty printer ────────────────────────────────────────────────────────────

def hexdump(data: bytes, indent: str = "    ") -> str:
    lines = []
    for off in range(0, len(data), 16):
        chunk      = data[off:off + 16]
        hex_part   = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{indent}{off:04x}  {hex_part:<48}  {ascii_part}")
    return "\n".join(lines)


def print_packet(pkt: dict, src_addr: tuple, raw_payload: bytes, show_hex: bool, channels: dict, hash_names: dict):
    now   = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    ptype = PAYLOAD_TYPE.get(pkt["payload_type"], f"0x{pkt['payload_type']:02X}")
    rtype = ROUTE_TYPE.get(pkt["route_type"],     f"0x{pkt['route_type']:02X}")

    print(f"\n{BOLD}{CYAN}[{now}]{RESET} {BOLD}{src_addr[0]}{RESET}")
    print(f"  {BOLD}Type:{RESET}  {YELLOW}{ptype}{RESET}  {DIM}({rtype}, ver={pkt['payload_ver']}){RESET}")

    if pkt["transport_codes"]:
        tc = pkt["transport_codes"]
        print(f"  {BOLD}Transport:{RESET} 0x{tc[0]:04X} / 0x{tc[1]:04X}")

    if pkt["path"]:
        path_str = " → ".join(pkt["path"])
        print(f"  {BOLD}Path:{RESET}  [{path_str}]  {DIM}(hash_size={pkt['hash_size']}){RESET}")

    payload      = pkt["payload"]
    payload_type = pkt["payload_type"]

    if payload_type == 0x04:  # ADVERT
        d    = decode_advert(payload)
        name = d.get("name", "")
        print(f"  {BOLD}Node:{RESET}  {GREEN}{name or '(unnamed)'}{RESET}  {DIM}[{d.get('node_type','')}]{RESET}")
        print(f"  {BOLD}ID:{RESET}    {d.get('node_id','?')}...  {DIM}(pub_key prefix){RESET}")
        print(f"  {BOLD}Time:{RESET}  {d.get('time_utc','?')}")
        if "lat" in d:
            print(f"  {BOLD}Loc:{RESET}   {d['lat']:.6f}, {d['lon']:.6f}")

    elif payload_type in (0x05, 0x06):  # GRP_TXT / GRP_DATA
        raw_hash   = payload[0] if payload else None
        known_name = hash_names.get(raw_hash)
        hash_label = f"0x{raw_hash:02x}" if raw_hash is not None else "?"
        ch_label   = f"{GREEN}{known_name}{RESET} {DIM}({hash_label}){RESET}" if known_name else f"{YELLOW}{hash_label}{RESET} {DIM}(unknown){RESET}"
        print(f"  {BOLD}Channel:{RESET} {ch_label}")

        ch_name, plaintext = try_decrypt_group(payload, channels)
        if plaintext is not None:
            if payload_type == 0x05:
                d = parse_grp_txt_plaintext(plaintext)
                print(f"  {BOLD}Time:{RESET}    {d.get('time_utc','?')}")
                print(f"  {BOLD}Message:{RESET} {d.get('text', '?')}")
            else:
                print(f"  {DIM}(GRP_DATA, decrypted, {len(plaintext)} bytes){RESET}")
                if show_hex:
                    print(hexdump(plaintext))
        elif not channels:
            mac = payload[1:3].hex() if len(payload) >= 3 else "?"
            print(f"  {DIM}mac={mac}  enc_len={len(payload)-3}  (use --channel NAME:SECRET_HEX to decrypt){RESET}")

    elif payload_type in (0x00, 0x01, 0x02, 0x08):  # REQ, RESPONSE, TXT_MSG, PATH
        d = decode_encrypted_header(payload, ptype)
        for k, v in d.items():
            print(f"  {BOLD}{k}:{RESET} {v}")

    elif payload_type == 0x07:  # ANON_REQ
        d = decode_anon_req(payload)
        for k, v in d.items():
            print(f"  {BOLD}{k}:{RESET} {v}")

    elif payload_type == 0x03:  # ACK
        print(f"  {DIM}(acknowledgement, {len(payload)} bytes){RESET}")

    else:
        print(f"  {BOLD}len:{RESET} {len(payload)}")
        if show_hex:
            print(hexdump(payload))

    if show_hex:
        print(f"  {DIM}--- raw mesh packet ({len(raw_payload)} bytes) ---{RESET}")
        print(hexdump(raw_payload))


# ── Argument parsing ──────────────────────────────────────────────────────────

def channel_hash_byte(secret: bytes) -> int:
    """
    Mirrors BaseChatMesh::setChannel() hash derivation:
      SHA256(secret[0:16])[0]  if secret[16:32] == all zeros (128-bit key)
      SHA256(secret[0:32])[0]  otherwise (256-bit key)
    """
    if secret[16:32] == bytes(16):
        return hashlib.sha256(secret[:16]).digest()[0]
    return hashlib.sha256(secret).digest()[0]


def parse_channels(channel_args: list[str]) -> tuple[dict, dict]:
    """
    Parse --channel NAME:HEXSECRET args.
    Returns (name→secret, hash_byte→name).
    """
    channels  = {}   # name → secret bytes
    hash_names = {}  # hash byte → name
    for arg in channel_args or []:
        if ":" not in arg:
            print(f"{RED}--channel must be NAME:SECRET_HEX (e.g. Default:aabb...){RESET}", file=sys.stderr)
            sys.exit(1)
        name, hex_secret = arg.split(":", 1)
        try:
            secret = bytes.fromhex(hex_secret)
        except ValueError:
            print(f"{RED}Invalid hex secret for channel '{name}'{RESET}", file=sys.stderr)
            sys.exit(1)
        if len(secret) not in (16, PUB_KEY_SIZE):
            print(f"{RED}Channel secret must be 16 or {PUB_KEY_SIZE} bytes, got {len(secret)}{RESET}", file=sys.stderr)
            sys.exit(1)
        if len(secret) == 16:
            secret = secret + bytes(16)   # zero-pad to 32 bytes
        channels[name]   = secret
        h                = channel_hash_byte(secret)
        hash_names[h]    = name
    return channels, hash_names


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="MeshCore EthernetBridge packet monitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--port",    type=int, default=5005,    help="UDP listen port (default: 5005)")
    parser.add_argument("--bind",    default="0.0.0.0",         help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--hex",     action="store_true",        help="Show hex dump of packet payloads")
    parser.add_argument("--channel", action="append", metavar="NAME:SECRET_HEX",
                        help="Channel secret for decryption (32 bytes = 64 hex chars). Repeat for multiple channels.")
    args = parser.parse_args()

    channels, hash_names = parse_channels(args.channel)

    if channels and not AES_AVAILABLE:
        print(f"{YELLOW}Warning: pycryptodome not installed — channel decryption disabled.{RESET}")
        print(f"{YELLOW}Install with: pip install pycryptodome{RESET}\n")
        channels = {}

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))

    print(f"{BOLD}MeshCore bridge monitor{RESET}  listening on {args.bind}:{args.port}")
    if channels:
        print(f"{DIM}Channels: {', '.join(channels.keys())}{RESET}")
    elif not AES_AVAILABLE:
        print(f"{DIM}(install pycryptodome to enable channel decryption){RESET}")
    else:
        print(f"{DIM}(use --channel NAME:SECRET_HEX to decrypt group messages){RESET}")
    print()

    stats = {"total": 0, "bad": 0}

    try:
        while True:
            data, addr = sock.recvfrom(512)
            stats["total"] += 1

            raw_mesh = parse_frame(data)
            if raw_mesh is None:
                stats["bad"] += 1
                continue

            pkt = decode_packet(raw_mesh)
            if pkt is None:
                print(f"{RED}Failed to decode mesh packet{RESET}")
                stats["bad"] += 1
                continue

            print_packet(pkt, addr, raw_mesh, args.hex, channels, hash_names)

    except KeyboardInterrupt:
        print(f"\n{DIM}Received {stats['total']} packets, {stats['bad']} bad.{RESET}")
        sys.exit(0)


if __name__ == "__main__":
    main()
