#!/usr/bin/env python3
"""
3GP 파일에서 AMR-WB 오디오 프레임과 H.264 비디오 NAL을 추출한다.
출력:
  {basename}_audio.amrwb  — AMR-WB raw 프레임 (RTP 페이로드 연속)
  {basename}_video.h264   — Annex-B H.264 NAL 스트림 (00 00 00 01 + NAL)

Usage:
  python3 extract_frames.py input.3gp [output_dir]
"""
import struct, sys, os


# ───────────────────── 3GP / MP4 box parser ─────────────────────

def read_boxes(data, start, end):
    """Yield (offset, size, box_type, data_start) for each box."""
    pos = start
    while pos + 8 <= end:
        size = struct.unpack_from('>I', data, pos)[0]
        btype = data[pos+4:pos+8].decode('ascii', errors='replace')
        hdr_len = 8
        if size == 1 and pos + 16 <= end:
            size = struct.unpack_from('>Q', data, pos+8)[0]
            hdr_len = 16
        elif size == 0:
            size = end - pos
        if size < 8:
            break
        yield (pos, size, btype, pos + hdr_len)
        pos += size


CONTAINER_TYPES = {'moov', 'trak', 'mdia', 'minf', 'stbl', 'edts', 'dinf'}


def find_box(data, start, end, target_path):
    """Navigate box hierarchy by path like 'moov/trak/mdia/minf/stbl/stsd'."""
    parts = target_path.split('/', 1)
    for pos, size, btype, dstart in read_boxes(data, start, end):
        if btype == parts[0]:
            if len(parts) == 1:
                return (pos, size, dstart)
            if btype in CONTAINER_TYPES:
                return find_box(data, dstart, pos + size, parts[1])
    return None


def find_all(data, start, end, target_type):
    """Find all boxes of a type within a range."""
    results = []
    for pos, size, btype, dstart in read_boxes(data, start, end):
        if btype == target_type:
            results.append((pos, size, dstart))
        if btype in CONTAINER_TYPES:
            results.extend(find_all(data, dstart, pos + size, target_type))
    return results


# ───────────────────── track parsing ─────────────────────

def parse_stsd_codec(data, stbl_start, stbl_end):
    """Return codec FourCC from stsd box."""
    for pos, size, btype, dstart in read_boxes(data, stbl_start, stbl_end):
        if btype == 'stsd':
            # version(4) + entry_count(4)
            off = dstart + 8
            esize = struct.unpack_from('>I', data, off)[0]
            etype = data[off+4:off+8].decode('ascii', errors='replace')
            return etype, off + 8, off + esize
    return None, 0, 0


def parse_stsz(data, stbl_start, stbl_end):
    """Return list of sample sizes."""
    for pos, size, btype, dstart in read_boxes(data, stbl_start, stbl_end):
        if btype == 'stsz':
            off = dstart + 4  # version+flags
            uniform = struct.unpack_from('>I', data, off)[0]
            count = struct.unpack_from('>I', data, off+4)[0]
            if uniform > 0:
                return [uniform] * count
            sizes = []
            o = off + 8
            for _ in range(count):
                sizes.append(struct.unpack_from('>I', data, o)[0])
                o += 4
            return sizes
    return []


def parse_stco(data, stbl_start, stbl_end):
    """Return list of chunk offsets."""
    for pos, size, btype, dstart in read_boxes(data, stbl_start, stbl_end):
        if btype in ('stco', 'co64'):
            off = dstart + 4
            count = struct.unpack_from('>I', data, off)[0]
            offsets = []
            o = off + 4
            fmt = '>I' if btype == 'stco' else '>Q'
            step = 4 if btype == 'stco' else 8
            for _ in range(count):
                offsets.append(struct.unpack_from(fmt, data, o)[0])
                o += step
            return offsets
    return []


def parse_stsc(data, stbl_start, stbl_end):
    """Return list of (first_chunk, samples_per_chunk, sample_desc_index)."""
    for pos, size, btype, dstart in read_boxes(data, stbl_start, stbl_end):
        if btype == 'stsc':
            off = dstart + 4
            count = struct.unpack_from('>I', data, off)[0]
            entries = []
            o = off + 4
            for _ in range(count):
                fc, spc, sdi = struct.unpack_from('>III', data, o)
                entries.append((fc, spc, sdi))
                o += 12
            return entries
    return []


def build_sample_offsets(chunk_offsets, stsc_entries, sample_sizes):
    """Build a list of (offset, size) for every sample by combining stco+stsc+stsz."""
    # Build chunk→samples_per_chunk mapping
    total_chunks = len(chunk_offsets)
    chunk_spc = [0] * (total_chunks + 1)

    for i, (fc, spc, _) in enumerate(stsc_entries):
        next_fc = stsc_entries[i+1][0] if i+1 < len(stsc_entries) else total_chunks + 1
        for c in range(fc, min(next_fc, total_chunks + 1)):
            chunk_spc[c] = spc

    sample_idx = 0
    result = []
    for ci in range(1, total_chunks + 1):
        offset = chunk_offsets[ci - 1]
        spc = chunk_spc[ci]
        for _ in range(spc):
            if sample_idx >= len(sample_sizes):
                break
            sz = sample_sizes[sample_idx]
            result.append((offset, sz))
            offset += sz
            sample_idx += 1

    return result


def extract_avc_sps_pps(data, entry_start, entry_end):
    """Extract SPS/PPS from avcC box inside stsd visual sample entry."""
    # Skip visual sample entry header (78 bytes after size+type)
    # 6(reserved)+2(dri)+2+2+12+2(w)+2(h)+4+4+4+2+32+2+2 = 78
    off = entry_start + 78
    sps_list, pps_list = [], []
    for pos, size, btype, dstart in read_boxes(data, off, entry_end):
        if btype == 'avcC' and size > 14:
            d = dstart
            _cfg_ver = data[d]; d += 1
            _profile = data[d]; d += 1
            _compat = data[d]; d += 1
            _level = data[d]; d += 1
            nal_len_size = (data[d] & 0x03) + 1; d += 1
            num_sps = data[d] & 0x1F; d += 1
            for _ in range(num_sps):
                sps_len = struct.unpack_from('>H', data, d)[0]; d += 2
                sps_list.append(data[d:d+sps_len]); d += sps_len
            num_pps = data[d] & 0xFF; d += 1
            for _ in range(num_pps):
                pps_len = struct.unpack_from('>H', data, d)[0]; d += 2
                pps_list.append(data[d:d+pps_len]); d += pps_len
            return sps_list, pps_list, nal_len_size
    return [], [], 4


# ───────────────────── main ─────────────────────

def extract(input_path, output_dir=None):
    with open(input_path, 'rb') as f:
        data = f.read()

    if output_dir is None:
        output_dir = os.path.dirname(input_path)
    basename = os.path.splitext(os.path.basename(input_path))[0]

    # Find all trak boxes
    traks = find_all(data, 0, len(data), 'trak')
    audio_out = None
    video_out = None

    for trak_pos, trak_size, trak_dstart in traks:
        trak_end = trak_pos + trak_size

        # Find stbl
        stbl = find_box(data, trak_dstart, trak_end, 'mdia/minf/stbl')
        if not stbl:
            continue
        stbl_pos, stbl_size, stbl_dstart = stbl
        stbl_end = stbl_pos + stbl_size

        codec, entry_start, entry_end = parse_stsd_codec(data, stbl_dstart, stbl_end)
        if not codec:
            continue

        sizes = parse_stsz(data, stbl_dstart, stbl_end)
        offsets = parse_stco(data, stbl_dstart, stbl_end)
        stsc = parse_stsc(data, stbl_dstart, stbl_end)
        samples = build_sample_offsets(offsets, stsc, sizes)

        if codec == 'sawb':
            # AMR-WB audio
            out_path = os.path.join(output_dir, f'{basename}_audio.amrwb')
            with open(out_path, 'wb') as out:
                for off, sz in samples:
                    out.write(data[off:off+sz])
            audio_out = out_path
            print(f'  Audio: {len(samples)} frames, {sum(s for _, s in samples)} bytes → {out_path}')

        elif codec == 'avc1':
            # H.264 video — convert from MP4 length-prefixed NALs to Annex-B
            sps_list, pps_list, nal_len_size = extract_avc_sps_pps(data, entry_start, entry_end)
            out_path = os.path.join(output_dir, f'{basename}_video.h264')
            start_code = b'\x00\x00\x00\x01'
            with open(out_path, 'wb') as out:
                # Write SPS/PPS first
                for sps in sps_list:
                    out.write(start_code + sps)
                for pps in pps_list:
                    out.write(start_code + pps)
                # Write NAL units
                for off, sz in samples:
                    pos = off
                    end = off + sz
                    while pos + nal_len_size <= end:
                        if nal_len_size == 4:
                            nal_sz = struct.unpack_from('>I', data, pos)[0]
                        elif nal_len_size == 2:
                            nal_sz = struct.unpack_from('>H', data, pos)[0]
                        else:
                            nal_sz = data[pos]
                        pos += nal_len_size
                        if pos + nal_sz > end:
                            break
                        out.write(start_code + data[pos:pos+nal_sz])
                        pos += nal_sz
            video_out = out_path
            print(f'  Video: {len(samples)} frames, SPS={len(sps_list)} PPS={len(pps_list)} → {out_path}')

    return audio_out, video_out


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} input.3gp [output_dir]')
        sys.exit(1)
    inp = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    print(f'Extracting: {inp}')
    a, v = extract(inp, outdir)
    if not a and not v:
        print('  No tracks found!')
