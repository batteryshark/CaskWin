"""Read-only, version-pinned SDL audio metadata inspection (never invokes target code).

Only mapped audio-module code, selected SDL device metadata, and explicitly
documented FAudio scalar fields may be read. No PCM, environment, stacks, or
unrelated process memory is collected. Requires pefile and capstone.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path
import struct
import time
import pefile
import capstone

SDL_HASH = '69707d7e699802fdd5811b48f5c9bd6fd57c84a3e4c6e731ad068b52413aaddc'
FAUDIO_HASH = '1dcf5aee1f6b8379b563ad35350cb1c5266cddd11ffd4cf2f8edc55eee96ced6'
K = C.WinDLL('kernel32', use_last_error=True)
K.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
K.OpenProcess.restype = W.HANDLE
K.CloseHandle.argtypes = [W.HANDLE]
K.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
K.ReadProcessMemory.restype = W.BOOL
K.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
K.QueryFullProcessImageNameW.restype = W.BOOL


class Reader:
    def __init__(self, pid, app):
        # PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ only.
        self.handle = K.OpenProcess(0x1000 | 0x10, False, pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        path = C.create_unicode_buffer(32768)
        size = W.DWORD(len(path))
        if not K.QueryFullProcessImageNameW(self.handle, 0, path, C.byref(size)):
            raise C.WinError(C.get_last_error())
        if Path(path.value).resolve() != (app / 'BlossomTales2.exe').resolve():
            raise RuntimeError('PID is not the exact staged Blossom executable')
        self.bytes_read = 0

    def read(self, address, size):
        if not address or size < 1 or size > 512 or self.bytes_read + size > 128 * 1024:
            raise RuntimeError('Read outside bounded audio-inspection budget')
        buffer = C.create_string_buffer(size)
        actual = C.c_size_t()
        if not K.ReadProcessMemory(self.handle, address, buffer, size, C.byref(actual)) or actual.value != size:
            raise C.WinError(C.get_last_error())
        self.bytes_read += size
        return buffer.raw

    def u64(self, address):
        return struct.unpack('<Q', self.read(address, 8))[0]


def module(path, base, expected_hash):
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash:
        raise RuntimeError('Unsupported audio DLL hash: ' + str(path))
    pe = pefile.PE(str(path))
    return {'base': base, 'size': pe.OPTIONAL_HEADER.SizeOfImage, 'pe': pe, 'hash': expected_hash}


def inside(mod, address, count=1):
    return mod['base'] <= address and address + count <= mod['base'] + mod['size']


def getter(reader, mod, name, dynamic=False, count=96):
    exported = next(x.address for x in mod['pe'].DIRECTORY_ENTRY_EXPORT.symbols if x.name == name.encode())
    address = mod['base'] + exported
    if dynamic:
        stub = reader.read(address, 7)
        if stub[:3] != b'\x48\xff\x25':
            raise RuntimeError('Unexpected SDL dynamic API jump')
        slot = address + 7 + struct.unpack('<i', stub[3:])[0]
        if not inside(mod, slot, 8):
            raise RuntimeError('SDL dynamic API slot outside SDL module')
        address = reader.u64(slot)
    if not inside(mod, address, count):
        raise RuntimeError('Audio getter outside expected module')
    code = reader.read(address, count)
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    instructions = []
    for instruction in disassembler.disasm(code, address):
        instructions.append({'rva': hex(instruction.address - mod['base']), 'op': instruction.mnemonic, 'args': instruction.op_str})
        if instruction.mnemonic.endswith('ret'):
            break
    return address, code, instructions


def audio_devices(reader, sdl, faudio):
    # Struct provenance: SDL release-2.24.0/src/audio/SDL_sysaudio.h and
    # FAudio 22.07/src/FAudio_internal.h. Getter disassembly above validates
    # device-table, enabled/callback, engine-quantum, format and samples offsets.
    address, code, _ = getter(reader, sdl, 'SDL_GetAudioDeviceStatus', True, 64)
    # Exact 2.24 x64 getter: bounds-check 16 slots then load open_devices[id-1].
    if code[5:13] != bytes.fromhex('83e90183f90f774b') or code[13:16] != b'\x48\x8d\x05':
        raise RuntimeError('Unexpected SDL device-table accessor')
    table = address + 20 + struct.unpack('<i', code[16:20])[0]
    if not inside(sdl, table, 16 * 8):
        raise RuntimeError('SDL device table outside module')
    devices = []
    for index, pointer in enumerate(struct.unpack('<16Q', reader.read(table, 128))):
        if not pointer:
            continue
        data = reader.read(pointer, 96)
        identity = struct.unpack_from('<I', data)[0]
        shutdown, enabled, paused, capture = struct.unpack_from('<4I', data, 80)
        if identity != index + 1 or capture or any(x > 1 for x in (shutdown, enabled, paused)):
            raise RuntimeError('Unexpected SDL device metadata; refusing further reads')
        def spec(offset):
            freq, fmt, channels, silence, frames, padding, size, callback, user = struct.unpack_from('<iHBBHHIQQ', data, offset)
            if not 8000 <= freq <= 192000 or not 1 <= channels <= 8 or not 1 <= frames <= 32768:
                raise RuntimeError('Unexpected SDL audio spec')
            return {'rate': freq, 'format': fmt, 'channels': channels, 'frames': frames, 'bytes': size}, callback, user
        device_spec, _, _ = spec(8)
        callback_spec, callback, audio = spec(40)
        if not inside(faudio, callback, 32):
            raise RuntimeError('SDL callback not inside verified FAudio image')
        # No indirect callback code is invoked or interpreted beyond module proof.
        local = faudio['pe'].get_data(callback - faudio['base'], 32)
        if reader.read(callback, 32) != local:
            raise RuntimeError('FAudio render callback differs from copied file')
        engine = reader.read(audio, 32)
        version, active = struct.unpack_from('<BB', engine)
        flags, quantum, master, sources = struct.unpack_from('<IIQQ', engine, 8)
        if quantum != callback_spec['frames'] or active > 1:
            raise RuntimeError('FAudio engine does not match SDL userdata')
        voices = []
        seen = set()
        while sources:
            if sources in seen or len(seen) >= 32:
                raise RuntimeError('FAudio source list exceeded bounded inspection')
            seen.add(sources)
            voice, sources = struct.unpack('<QQ', reader.read(sources, 16))
            owner, voice_flags, voice_type = struct.unpack('<QII', reader.read(voice, 16))
            if owner != audio or voice_type != 0:
                raise RuntimeError('Source does not belong to validated FAudio engine')
            fmt = reader.u64(voice + 0xf8)
            format_tag, channels, rate = struct.unpack('<HHI', reader.read(fmt, 8))
            if not 8000 <= rate <= 192000 or not 1 <= channels <= 8:
                raise RuntimeError('Unexpected FAudio source format')
            voices.append({'pointer': voice, 'rate': rate, 'channels': channels, 'formatTag': format_tag})
        devices.append({'id': identity, 'shutdown': shutdown, 'enabled': enabled, 'paused': paused,
                        'deviceSpec': device_spec, 'callbackSpec': callback_spec,
                        'conversionStreamPresent': bool(struct.unpack_from('<Q', data, 72)[0]),
                        'engineActive': active, 'engineFlags': flags, 'quantumFrames': quantum,
                        'callbackRva': hex(callback - faudio['base']), 'audio': audio, 'voices': voices})
    return devices


def sample_progress(reader, devices, seconds):
    start = time.perf_counter()
    rows = []
    while True:
        elapsed = time.perf_counter() - start
        row = {'elapsedSeconds': round(elapsed, 6), 'voices': []}
        for device in devices:
            for index, voice in enumerate(device['voices']):
                if reader.u64(voice['pointer']) != device['audio']:
                    raise RuntimeError('Source lifetime changed during snapshot sampling')
                data = reader.read(voice['pointer'] + 0x118, 32)
                active = data[0]
                ratio = struct.unpack_from('<f', data, 4)[0]
                samples, queue = struct.unpack_from('<QQ', data, 16)
                row['voices'].append({'device': device['id'], 'voice': index, 'active': active,
                                      'frequencyRatio': ratio, 'samplesPlayed': samples, 'bufferPresent': bool(queue)})
        rows.append(row)
        if elapsed >= seconds:
            break
        time.sleep(.05)
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid', type=int, required=True)
    p.add_argument('--app', type=Path, required=True)
    p.add_argument('--sdl-base', type=lambda x: int(x, 0), required=True)
    p.add_argument('--faudio-base', type=lambda x: int(x, 0), required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--sample-seconds', type=int, choices=range(0, 11), default=0)
    args = p.parse_args()
    reader = Reader(args.pid, args.app)
    try:
        sdl = module(args.app / 'SDL2.dll', args.sdl_base, SDL_HASH)
        faudio = module(args.app / 'FAudio.dll', args.faudio_base, FAUDIO_HASH)
        driver_address, code, driver_code = getter(reader, sdl, 'SDL_GetCurrentAudioDriver', True)
        if code[:3] != b'\x48\x8b\x05' or code[7] != 0xc3:
            raise RuntimeError('Unknown current-audio-driver getter; no speculative memory reads')
        state = driver_address + 7 + struct.unpack('<i', code[3:7])[0]
        if not inside(sdl, state, 8):
            raise RuntimeError('SDL audio state outside SDL module')
        name = reader.u64(state)
        if not inside(sdl, name, 32):
            raise RuntimeError('SDL backend string outside SDL module')
        driver = reader.read(name, 32).split(b'\0')[0].decode('ascii')
        if driver not in ('wasapi', 'directsound', 'winmm', 'dummy', 'disk'):
            raise RuntimeError('Unexpected backend string')
        report = {'observedUtc': datetime.now(timezone.utc).isoformat(),
                  'pid': args.pid, 'backend': driver, 'sdlSha256': SDL_HASH, 'faudioSha256': FAUDIO_HASH,
                  'targetCodeInvoked': False, 'targetMemoryWritten': False, 'audioCaptured': False,
                  'getters': {'SDL_GetCurrentAudioDriver': driver_code}}
        for name in ('SDL_GetAudioDeviceStatus', 'SDL_GetQueuedAudioSize'):
            report['getters'][name] = getter(reader, sdl, name, True, 192)[2]
        for name in ('FAudio_GetProcessingQuantum', 'FAudioSourceVoice_GetState', 'FAudioVoice_GetVoiceDetails'):
            report['getters'][name] = getter(reader, faudio, name, False, 192)[2]
        devices = audio_devices(reader, sdl, faudio)
        if args.sample_seconds:
            report['progressSnapshots'] = sample_progress(reader, devices, args.sample_seconds)
        for device in devices:
            del device['audio']
            for voice in device['voices']:
                del voice['pointer']
        report['devices'] = devices
        report['limitations'] = ['Read-only snapshots are not synchronized with the mixer',
                                'Source counters measure mixing progress, not endpoint playback or callback deadlines',
                                'No hardware underrun or audible-glitch claim follows from this inspection']
        report['bytesRead'] = reader.bytes_read
        args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(json.dumps(report, indent=2))
    finally:
        K.CloseHandle(reader.handle)


if __name__ == '__main__':
    main()
