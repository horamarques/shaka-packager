#!/usr/bin/env python3
"""Synthesize bear-640x360-scte35.ts from bear-640x360.ts.

- Rewrites the PMT: adds an ES entry (stream_type 0x86, PID 0x102) whose
  ES_info carries a registration descriptor (tag 0x05) with "CUEI", updating
  section_length and CRC-32/MPEG.
- Injects one TS packet on PID 0x102 carrying a splice_info_section():
  time_signal at PTS 1.0s with a CUEI segmentation descriptor,
  segmentation_type_id 0x30 (Provider Ad Start -> cue-out), duration 30s.
"""
import struct, sys

SRC, DST = sys.argv[1], sys.argv[2]

def crc32_mpeg(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if (crc & 0x80000000) else (crc << 1)
            crc &= 0xFFFFFFFF
    return crc

class BitWriter:
    def __init__(self): self.bits = []
    def w(self, val, n):
        for i in range(n - 1, -1, -1): self.bits.append((val >> i) & 1)
    def bytes(self):
        assert len(self.bits) % 8 == 0, "not byte aligned"
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            v = 0
            for b in self.bits[i:i+8]: v = (v << 1) | b
            out.append(v)
        return bytes(out)

data = open(SRC, "rb").read()
pkts = [bytearray(data[i:i+188]) for i in range(0, len(data), 188)]
pid = lambda p: ((p[1] & 0x1F) << 8) | p[2]

# ---- locate PMT PID from PAT ----
pat = next(p for p in pkts if pid(p) == 0 and (p[1] & 0x40))
off = 4 + (1 + pat[4] if (pat[3] >> 4) & 2 else 0)
off += 1 + pat[off]
pmt_pid = ((pat[off + 10] & 0x1F) << 8) | pat[off + 11]

SCTE_PID = 0x102

# ---- rewrite PMT ----
for idx, p in enumerate(pkts):
    if pid(p) == pmt_pid and (p[1] & 0x40):
        off = 4 + (1 + p[4] if (p[3] >> 4) & 2 else 0)
        ptr_off = off
        off += 1 + p[off]
        slen = ((p[off + 1] & 0x0F) << 8) | p[off + 2]
        sec = bytes(p[off : off + 3 + slen])
        body = bytearray(sec[:-4])  # strip old CRC
        # new ES entry: stream_type 0x86, PID 0x102, ES_info_len 6:
        #   registration descriptor tag 0x05 len 4 "CUEI"
        es = bytes([0x86, 0xE0 | (SCTE_PID >> 8), SCTE_PID & 0xFF,
                    0xF0, 0x06, 0x05, 0x04]) + b"CUEI"
        body += es
        new_slen = slen + len(es)
        body[1] = (body[1] & 0xF0) | ((new_slen >> 8) & 0x0F)
        body[2] = new_slen & 0xFF
        body += struct.pack(">I", crc32_mpeg(bytes(body)))
        newsec = bytes(body)
        room = 188 - (ptr_off + 1)
        assert len(newsec) <= room, "PMT does not fit"
        p[ptr_off] = 0  # pointer_field
        p[ptr_off + 1 : ptr_off + 1 + len(newsec)] = newsec
        for i in range(ptr_off + 1 + len(newsec), 188): p[i] = 0xFF
        pmt_index = idx
        print(f"PMT rewritten at pkt {idx}: section_length {slen} -> {new_slen}")
        break
else:
    sys.exit("PMT not found")

# ---- build splice_info_section: time_signal @ 1.0s, seg 0x30, 30s ----
bw = BitWriter()
bw.w(0, 8)                    # protocol_version
bw.w(0, 1); bw.w(0, 6)        # encrypted_packet, encryption_algorithm
bw.w(0, 33)                   # pts_adjustment
bw.w(0, 8)                    # cw_index
bw.w(0xFFF, 12)               # tier
# time_signal(): splice_time with time_specified
ts_cmd = BitWriter()
ts_cmd.w(1, 1); ts_cmd.w(0x3F, 6); ts_cmd.w(90000, 33)  # pts_time = 1.0s
cmd_bytes = ts_cmd.bytes()
bw.w(len(cmd_bytes), 12)      # splice_command_length
bw.w(0x06, 8)                 # splice_command_type = time_signal
for b in cmd_bytes: bw.w(b, 8)
# segmentation_descriptor
sd = BitWriter()
sd.w(0x43554549, 32)          # CUEI
sd.w(1234, 32)                # segmentation_event_id
sd.w(0, 1); sd.w(0x7F, 7)     # cancel=0, reserved
sd.w(1, 1)                    # program_segmentation_flag
sd.w(1, 1)                    # segmentation_duration_flag
sd.w(1, 1)                    # delivery_not_restricted_flag
sd.w(0x1F, 5)                 # reserved
sd.w(30 * 90000, 40)          # segmentation_duration = 30s
sd.w(0, 8); sd.w(0, 8)        # upid_type, upid_length
sd.w(0x30, 8)                 # segmentation_type_id = Provider Ad Start
sd.w(0, 8); sd.w(0, 8)        # segment_num, segments_expected
sd_bytes = sd.bytes()
bw.w(0x02, 8)                 # splice_descriptor_tag
bw.w(len(sd_bytes), 8)        # descriptor_length
for b in sd_bytes: bw.w(b, 8)
# descriptor_loop_length goes BEFORE the descriptors: rebuild in order
inner = bw.bytes()
# split: fixed header part ends after command; recompose with loop length
# (we wrote descriptors straight after command; need loop len between)
# Rebuild properly:
bw2 = BitWriter()
bw2.w(0, 8); bw2.w(0, 1); bw2.w(0, 6); bw2.w(0, 33); bw2.w(0, 8); bw2.w(0xFFF, 12)
bw2.w(len(cmd_bytes), 12); bw2.w(0x06, 8)
for b in cmd_bytes: bw2.w(b, 8)
desc_loop = bytes([0x02, len(sd_bytes)]) + sd_bytes
bw2.w(len(desc_loop), 16)
for b in desc_loop: bw2.w(b, 8)
payload = bw2.bytes()
section_length = len(payload) + 4  # + CRC
hdr = BitWriter()
hdr.w(0xFC, 8)                # table_id
hdr.w(0, 1); hdr.w(0, 1); hdr.w(3, 2)  # ssi=0, private=0, reserved
hdr.w(section_length, 12)
section = hdr.bytes() + payload
section += struct.pack(">I", crc32_mpeg(section))
print(f"splice_info_section: {len(section)} bytes")

# ---- wrap in a TS packet ----
sp = bytearray(188)
sp[0] = 0x47
sp[1] = 0x40 | (SCTE_PID >> 8)   # PUSI=1
sp[2] = SCTE_PID & 0xFF
sp[3] = 0x10                      # payload only, CC=0
sp[4] = 0                         # pointer_field
sp[5 : 5 + len(section)] = section
for i in range(5 + len(section), 188): sp[i] = 0xFF

# insert right after the PMT packet
pkts.insert(pmt_index + 1, sp)
open(DST, "wb").write(b"".join(bytes(p) for p in pkts))
print(f"wrote {DST}: {len(pkts)} packets")
