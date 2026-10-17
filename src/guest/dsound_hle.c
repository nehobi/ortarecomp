/*
 * DirectSound, replaced at its API.
 *
 * From Concept of Love (JSRF, XDK 4134); addresses are Orta's (XDK 4928),
 * mapped by XbSymbolDatabase name, each `ret N` checked in Orta's code.
 *
 * The title links the XDK's DirectSound library, which drives the Xbox's
 * MCPX audio processor directly; with no such hardware its DirectSoundCreate
 * fails with DSERR_NODRIVER and the title quits. Every DirectSound entry
 * point the game itself calls is replaced here and routed to our mixer
 * (src/audio/mixer.c). The library's internals are then never reached.
 *
 * Conventions: each function is __stdcall. At entry g_esp points at the
 * return address and argument n is at g_esp + 4 + 4n. Each one pops the
 * return address and exactly the argument bytes the original's `ret N` did
 * (measured from the binary), and returns its HRESULT in g_eax.
 *
 * Objects handed to the game are small blocks of guest memory holding a tag
 * and our ids; the game treats them as opaque interface pointers.
 *
 * Not implemented yet, accepted and recorded: 3D positioning, envelopes
 * (SetEG), LFOs, filters, I3DL2 reverb and DSP effects. PDO_DSOUND_TRACE=1
 * logs every call.
 */

#include "recomp_types.h"

#include "audio/mixer.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define ARG(n)      MEM32(g_esp + 4 + 4 * (n))
#define RETURN(argbytes, hr) do { g_eax = (uint32_t)(hr); g_esp += 4 + (argbytes); return; } while (0)

#define DS_OK               0x00000000u
#define DSERR_INVALIDCALL   0x88780032u
#define DSERR_OUTOFMEMORY   0x8007000Eu

#define TAG_DSOUND  0x534F4450u     /* "PDOS" */
#define TAG_BUFFER  0x464F4450u     /* "PDOF" */

/* Object layout in guest memory. */
#define OBJ_TAG     0
#define OBJ_VOICE   4
#define OBJ_REFS    8
#define OBJ_DATA    12   /* buffer memory we allocated, if any */
#define OBJ_BYTES   16
#define OBJ_SIZE    32

static uint32_t the_dsound;

static int trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("PDO_DSOUND_TRACE");
        on = v && *v && *v != '0';
    }
    return on;
}

#define TRACE(...) do { if (trace_on()) SDL_Log("dsound: " __VA_ARGS__); } while (0)

static void note_once(const char *what)
{
    /* Each unimplemented feature is reported the first time it is used. */
    static const char *seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == what)
            return;
        if (!seen[i]) {
            seen[i] = what;
            SDL_Log("dsound: %s used by the game (accepted, not implemented yet)", what);
            return;
        }
    }
}

static int voice_of(uint32_t obj)
{
    if (!obj || MEM32(obj + OBJ_TAG) != TAG_BUFFER)
        return 0;
    return (int)MEM32(obj + OBJ_VOICE);
}

static uint32_t guest_alloc(uint32_t bytes)
{
    uint32_t va = xbox_HeapAlloc(bytes ? bytes : 4, 32);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, bytes);
    return va;
}

/* ── IDirectSound ─────────────────────────────────────────────────────── */

/* DirectSoundCreate(pguid, ppDS, pUnkOuter) */
void sub_002C49ED(void)
{
    uint32_t pp = ARG(1);

    if (!the_dsound) {
        pdo_mixer_init();
        the_dsound = guest_alloc(OBJ_SIZE);
        if (!the_dsound)
            RETURN(12, DSERR_OUTOFMEMORY);
        MEM32(the_dsound + OBJ_TAG) = TAG_DSOUND;
    }
    MEM32(the_dsound + OBJ_REFS) += 1;
    if (pp)
        MEM32(pp) = the_dsound;
    TRACE("DirectSoundCreate -> 0x%08x", the_dsound);
    RETURN(12, DS_OK);
}

/* IDirectSound_AddRef(pDS) */
void sub_002C127D(void)
{
    uint32_t ds = ARG(0);
    RETURN(4, ds ? (MEM32(ds + OBJ_REFS) += 1) : 0);
}

/* IDirectSound_Release(pDS). The device lives for the whole run. */
void sub_002C1293(void)
{
    uint32_t ds = ARG(0), refs = 0;
    if (ds && MEM32(ds + OBJ_REFS))
        refs = (MEM32(ds + OBJ_REFS) -= 1);
    RETURN(4, refs);
}

/* IDirectSound_GetCaps(pDS, LPDSCAPS) */
void sub_002C240E(void)
{
    uint32_t caps = ARG(1);
    if (caps) {
        MEM32(caps + 0) = 192;      /* dwFree2DBuffers */
        MEM32(caps + 4) = 64;       /* dwFree3DBuffers */
        MEM32(caps + 8) = 2047;     /* dwFreeBufferSGEs */
        MEM32(caps + 12) = 0;       /* dwMemoryAllocated */
    }
    RETURN(8, DS_OK);
}

/* IDirectSound_CreateSoundBuffer(pDS, pdsbd, ppBuffer, pUnkOuter) */
void sub_002C4833(void)
{
    uint32_t desc = ARG(1), pp = ARG(2);
    uint32_t flags, bytes, wfx, mixbins, obj, data = 0;
    pdo_voice_desc vd;
    int id;

    if (!desc || !pp)
        RETURN(16, DSERR_INVALIDCALL);
    flags = MEM32(desc + 4);
    bytes = MEM32(desc + 8);
    wfx = MEM32(desc + 12);
    mixbins = MEM32(desc + 16);

    memset(&vd, 0, sizeof vd);
    vd.format = PDO_FMT_PCM16;
    vd.channels = 1;
    vd.sample_rate = 48000;
    vd.positional = (flags & 0x10) != 0;    /* DSBCAPS_CTRL3D */
    if (wfx) {
        uint16_t tag = MEM16(wfx), bits = MEM16(wfx + 14);
        vd.channels = MEM16(wfx + 2) == 2 ? 2 : 1;
        vd.sample_rate = MEM32(wfx + 4);
        if (tag == 0x0069) {
            vd.format = PDO_FMT_XADPCM;
            vd.samples_per_block = MEM16(wfx + 16) >= 2 ? MEM16(wfx + 18) : 64;
        } else {
            vd.format = bits == 8 ? PDO_FMT_PCM8 : PDO_FMT_PCM16;
            if (tag != 1)
                SDL_Log("dsound: unknown wave format 0x%04x, treating as PCM", tag);
        }
    }

    if (mixbins && MEM32(mixbins)) {
        uint32_t n = MEM32(mixbins), pairs = MEM32(mixbins + 4);
        for (uint32_t i = 0; i < n && i < PDO_MAX_VOICE_BINS && pairs; i++) {
            vd.bins[vd.bin_count].bin = MEM32(pairs + i * 8);
            vd.bins[vd.bin_count].volume = (int32_t)MEM32(pairs + i * 8 + 4);
            vd.bin_count++;
        }
    } else {
        /* The default routing: a mono buffer to both front speakers, a stereo
         * one left to left and right to right. */
        vd.bins[0].bin = PDO_BIN_FRONT_LEFT;
        vd.bins[1].bin = PDO_BIN_FRONT_RIGHT;
        vd.bin_count = 2;
    }

    if (bytes) {
        data = guest_alloc(bytes);
        if (!data)
            RETURN(16, DSERR_OUTOFMEMORY);
        vd.data = data;
        vd.bytes = bytes;
    }

    id = pdo_voice_create(&vd);
    obj = guest_alloc(OBJ_SIZE);
    if (!id || !obj)
        RETURN(16, DSERR_OUTOFMEMORY);
    MEM32(obj + OBJ_TAG) = TAG_BUFFER;
    MEM32(obj + OBJ_VOICE) = (uint32_t)id;
    MEM32(obj + OBJ_REFS) = 1;
    MEM32(obj + OBJ_DATA) = data;
    MEM32(obj + OBJ_BYTES) = bytes;
    MEM32(pp) = obj;

    TRACE("CreateSoundBuffer flags 0x%x, %u bytes, %s %u ch %u Hz, %d bins -> 0x%08x",
          flags, bytes, vd.format == PDO_FMT_XADPCM ? "adpcm" : vd.format == PDO_FMT_PCM8 ? "pcm8" : "pcm16",
          vd.channels, vd.sample_rate, vd.bin_count, obj);
    RETURN(16, DS_OK);
}

/* DirectSound HRTF setup (called once during sound init); our mixer needs
 * no tables. Takes no arguments. */
void sub_002C12DA(void)
{
    note_once("HRTF");
    g_esp += 4;
}

/* DirectSoundDoWork(): the mixer runs on its own thread. */
void sub_002C26A5(void)
{
    g_esp += 4;
}

/* IDirectSound_CommitDeferredSettings(pDS) */
void sub_002C379E(void) { RETURN(4, DS_OK); }

/* ── DSP effects image ─────────────────────────────────────────────────
 * The title loads a DSP program (reverb and the like) and then reads and
 * writes its parameters by effect index. We return a descriptor whose state
 * segments are plain memory, so parameter traffic works, and record it. */

#define FX_COUNT       32
#define FX_STATE_BYTES 4096
static uint32_t fx_desc;

/* IDirectSound_DownloadEffectsImage(pDS, pvImage, dwSize, pImageLoc, ppDesc) */
void sub_002C2446(void)
{
    uint32_t pp = ARG(4);

    note_once("DSP effects image (reverb)");
    if (!fx_desc) {
        fx_desc = guest_alloc(8 + FX_COUNT * 32);
        if (!fx_desc)
            RETURN(20, DSERR_OUTOFMEMORY);
        MEM32(fx_desc) = FX_COUNT;
        for (int i = 0; i < FX_COUNT; i++) {
            uint32_t map = fx_desc + 8 + (uint32_t)i * 32;
            MEM32(map + 8) = guest_alloc(FX_STATE_BYTES);    /* lpvStateSegment */
            MEM32(map + 12) = FX_STATE_BYTES;
        }
    }
    if (pp)
        MEM32(pp) = fx_desc;
    RETURN(20, DS_OK);
}

static uint32_t fx_state(uint32_t index, uint32_t offset, uint32_t size)
{
    uint32_t map;
    if (!fx_desc || index >= FX_COUNT || offset > FX_STATE_BYTES || size > FX_STATE_BYTES - offset)
        return 0;
    map = fx_desc + 8 + index * 32;
    return MEM32(map + 8) + offset;
}

/* IDirectSound_GetEffectData(pDS, index, offset, pvData, size) */
void sub_002C246D(void)
{
    uint32_t src = fx_state(ARG(1), ARG(2), ARG(4)), dst = ARG(3), n = ARG(4);
    if (!src || !dst)
        RETURN(20, DSERR_INVALIDCALL);
    for (uint32_t i = 0; i < n; i++)
        MEM8(dst + i) = MEM8(src + i);
    RETURN(20, DS_OK);
}

/* IDirectSound_SetEffectData(pDS, index, offset, pvData, size, dwApply) */
void sub_002C2494(void)
{
    uint32_t dst = fx_state(ARG(1), ARG(2), ARG(4)), src = ARG(3), n = ARG(4);
    if (!dst || !src)
        RETURN(24, DSERR_INVALIDCALL);
    for (uint32_t i = 0; i < n; i++)
        MEM8(dst + i) = MEM8(src + i);
    RETURN(24, DS_OK);
}

/* Listener 3D state, recorded only for now. */
void sub_002C43BD(void) { note_once("3D listener"); RETURN(12, DS_OK); } /* SetAllParameters */
void sub_002C43DD(void) { RETURN(12, DS_OK); }  /* SetDistanceFactor(pDS, f, apply) */
void sub_002C4401(void) { RETURN(12, DS_OK); }  /* SetDopplerFactor */
void sub_002C4425(void) { RETURN(32, DS_OK); }  /* SetOrientation(pDS, 6 floats, apply) */
void sub_002C446F(void) { RETURN(20, DS_OK); }  /* SetPosition(pDS, x, y, z, apply) */
void sub_002C44A4(void) { RETURN(12, DS_OK); }  /* SetRolloffFactor */
void sub_002C44C8(void) { RETURN(20, DS_OK); }  /* SetVelocity */
void sub_002C44FD(void) { note_once("I3DL2 reverb"); RETURN(12, DS_OK); }  /* SetI3DL2Listener */

/* ── IDirectSoundBuffer ───────────────────────────────────────────────── */

/* IDirectSoundBuffer_AddRef(pBuf) */
void sub_002C12AE(void)
{
    uint32_t b = ARG(0);
    RETURN(4, voice_of(b) ? (MEM32(b + OBJ_REFS) += 1) : 0);
}

/* IDirectSoundBuffer_Release(pBuf) */
void sub_002C12C4(void)
{
    uint32_t b = ARG(0), refs = 0;
    int id = voice_of(b);

    if (id && MEM32(b + OBJ_REFS)) {
        refs = (MEM32(b + OBJ_REFS) -= 1);
        if (!refs) {
            pdo_voice_destroy(id);
            MEM32(b + OBJ_TAG) = 0;
            if (MEM32(b + OBJ_DATA))
                xbox_HeapFree(MEM32(b + OBJ_DATA));
            xbox_HeapFree(b);
        }
    }
    RETURN(4, refs);
}

/* IDirectSoundBuffer_SetBufferData(pBuf, pvData, dwBytes): play the game's
 * own memory. */
void sub_002C451D(void)
{
    int id = voice_of(ARG(0));
    if (!id)
        RETURN(12, DSERR_INVALIDCALL);
    pdo_voice_set_data(id, ARG(1), ARG(2));
    TRACE("SetBufferData 0x%08x: 0x%08x, %u bytes", ARG(0), ARG(1), ARG(2));
    RETURN(12, DS_OK);
}

/* IDirectSoundBuffer_Lock(pBuf, offset, bytes, ppv1, pdw1, ppv2, pdw2, flags) */
void sub_002C2652(void)
{
    uint32_t b = ARG(0), off = ARG(1), n = ARG(2), flags = ARG(7);
    uint32_t data = MEM32(b + OBJ_DATA), size = MEM32(b + OBJ_BYTES);
    uint32_t n1, n2;
    int id = voice_of(b);

    if (!id || !data || !size)
        RETURN(32, DSERR_INVALIDCALL);
    if (flags & 2)              /* DSBLOCK_ENTIREBUFFER */
        off = 0, n = size;
    else if (flags & 1)         /* DSBLOCK_FROMWRITECURSOR */
        off = pdo_voice_position(id);
    off %= size;
    if (n > size)
        n = size;
    n1 = size - off < n ? size - off : n;
    n2 = n - n1;
    if (ARG(3)) MEM32(ARG(3)) = data + off;
    if (ARG(4)) MEM32(ARG(4)) = n1;
    if (ARG(5)) MEM32(ARG(5)) = n2 ? data : 0;
    if (ARG(6)) MEM32(ARG(6)) = n2;
    RETURN(32, DS_OK);
}

/* IDirectSoundBuffer_Unlock: nothing to do, the voice reads the memory. */
void sub_002C12A9(void) { RETURN(20, DS_OK); }

/* IDirectSoundBuffer_Play(pBuf, reserved1, reserved2, flags) */
void sub_002C2582(void)
{
    int id = voice_of(ARG(0));
    if (!id)
        RETURN(16, DSERR_INVALIDCALL);
    if (ARG(3) & 2)             /* DSBPLAY_FROMSTART */
        pdo_voice_set_position(id, 0);
    pdo_voice_play(id, (ARG(3) & 1) != 0);
    TRACE("Play 0x%08x flags 0x%x", ARG(0), ARG(3));
    RETURN(16, DS_OK);
}

/* IDirectSoundBuffer_Stop(pBuf) */
void sub_002C25A6(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_stop(id);
    RETURN(4, DS_OK);
}

/* IDirectSoundBuffer_GetStatus(pBuf, pdwStatus) */
void sub_002C25FA(void)
{
    int id = voice_of(ARG(0));
    bool looping = false, playing = id && pdo_voice_playing(id, &looping);
    if (ARG(1))
        MEM32(ARG(1)) = (playing ? 1u : 0u) | (playing && looping ? 4u : 0u);
    RETURN(8, id ? DS_OK : DSERR_INVALIDCALL);
}

/* IDirectSoundBuffer_GetCurrentPosition(pBuf, pdwPlay, pdwWrite) */
void sub_002C2616(void)
{
    int id = voice_of(ARG(0));
    uint32_t pos = id ? pdo_voice_position(id) : 0;
    if (ARG(1)) MEM32(ARG(1)) = pos;
    if (ARG(2)) MEM32(ARG(2)) = pos;
    RETURN(12, id ? DS_OK : DSERR_INVALIDCALL);
}

/* IDirectSoundBuffer_SetCurrentPosition(pBuf, dwOffset) */
void sub_002C2636(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_set_position(id, ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetLoopRegion(pBuf, dwStart, dwLength) */
void sub_002C25DA(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_set_loop_region(id, ARG(1), ARG(2));
    RETURN(12, DS_OK);
}

/* IDirectSoundBuffer_SetVolume(pBuf, lVolume) */
void sub_002C24BE(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_set_volume(id, (int32_t)ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetHeadroom(pBuf, dwHeadroom) */
void sub_002C252E(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_set_headroom(id, ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetFrequency(pBuf, dwFrequency) */
void sub_002C37B6(void)
{
    int id = voice_of(ARG(0));
    TRACE("SetFrequency 0x%08x %u Hz", ARG(0), ARG(1));
    if (id)
        pdo_voice_set_frequency(id, ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetMixBins(pBuf, pMixBins) */
void sub_002C254A(void)
{
    int id = voice_of(ARG(0));
    uint32_t mb = ARG(1);
    pdo_voice_bin bins[PDO_MAX_VOICE_BINS];
    int n = 0;

    if (id && mb) {
        uint32_t count = MEM32(mb), pairs = MEM32(mb + 4);
        for (uint32_t i = 0; i < count && n < PDO_MAX_VOICE_BINS && pairs; i++, n++) {
            bins[n].bin = MEM32(pairs + i * 8);
            bins[n].volume = (int32_t)MEM32(pairs + i * 8 + 4);
        }
        pdo_voice_set_bins(id, bins, n);
    }
    RETURN(8, DS_OK);
}

void sub_002C24F6(void) { note_once("envelopes (SetEG)"); RETURN(8, DS_OK); }
void sub_002C24DA(void) { note_once("LFOs (SetLFO)"); RETURN(8, DS_OK); }
void sub_002C2512(void) { note_once("filters (SetFilter)"); RETURN(8, DS_OK); }

/* Per-buffer 3D state, recorded only for now. */
void sub_002C37D2(void) { note_once("3D sound sources"); RETURN(12, DS_OK); } /* SetAllParameters */
void sub_002C37F2(void) { RETURN(16, DS_OK); }  /* SetConeAngles */
void sub_002C3816(void) { RETURN(20, DS_OK); }  /* SetConeOrientation */
void sub_002C384B(void) { RETURN(12, DS_OK); }  /* SetConeOutsideVolume */
void sub_002C386B(void) { RETURN(12, DS_OK); }  /* SetMaxDistance */
void sub_002C388F(void) { RETURN(12, DS_OK); }  /* SetMinDistance */
void sub_002C38B3(void) { RETURN(12, DS_OK); }  /* SetMode */
void sub_002C38D3(void) { RETURN(20, DS_OK); }  /* SetPosition */
void sub_002C3908(void) { RETURN(20, DS_OK); }  /* SetVelocity */
void sub_002C393D(void) { RETURN(12, DS_OK); }  /* SetDistanceFactor */
void sub_002C3961(void) { RETURN(12, DS_OK); }  /* SetDopplerFactor */
void sub_002C3985(void) { RETURN(12, DS_OK); }  /* SetRolloffFactor */
void sub_002C39A9(void) { RETURN(12, DS_OK); }  /* SetI3DL2Source */

/* ── Calls Orta makes that JSRF did not ───────────────────────────────── */

/* IDirectSound_GetSpeakerConfig(pDS, pdwSpeakerConfig): stereo output. */
void sub_002C242A(void)
{
    if (ARG(1))
        MEM32(ARG(1)) = 0x00000002u;    /* DSSPEAKER_STEREO */
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetMixBinVolumes(pBuf, pMixBins): new volumes for the
 * listed bins. Our voices hold only the bins they route to, so this sets
 * the same list (the game passes the whole set). */
void sub_002C2566(void)
{
    int id = voice_of(ARG(0));
    uint32_t mb = ARG(1);
    pdo_voice_bin bins[PDO_MAX_VOICE_BINS];
    int n = 0;

    if (id && mb) {
        uint32_t count = MEM32(mb), pairs = MEM32(mb + 4);
        for (uint32_t i = 0; i < count && n < PDO_MAX_VOICE_BINS && pairs; i++, n++) {
            bins[n].bin = MEM32(pairs + i * 8);
            bins[n].volume = (int32_t)MEM32(pairs + i * 8 + 4);
        }
        pdo_voice_set_bins(id, bins, n);
    }
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_Pause(pBuf, dwPause): DSBPAUSE_RESUME 0, DSBPAUSE_PAUSE
 * 1, DSBPAUSE_SYNCHPAUSE 2. Pausing keeps the position. */
static void pause_voice(int id, uint32_t how)
{
    bool looping = false;

    if (!id)
        return;
    pdo_voice_playing(id, &looping);
    if (how == 0)
        pdo_voice_play(id, looping);
    else
        pdo_voice_stop(id);
}

void sub_002C25BE(void)
{
    pause_voice(voice_of(ARG(0)), ARG(1));
    RETURN(8, DS_OK);
}

/* ── IDirectSoundStream ───────────────────────────────────────────────────
 * A stream is an XMediaObject: the game hands it packets with Process and
 * polls their status. Our stream objects carry the library's own vtable
 * (CDirectSoundStream, 0x0032DD9C), whose methods are replaced below, so
 * calls through the vtable and through the C wrappers both land here.
 *
 * Packets play back to back on a mixer voice (pdo_voice_queue). The
 * descriptor the game passes may be on its stack, so each packet's
 * pdwCompletedSize, pdwStatus and completion event are copied when it is
 * queued, and written from the mixer thread when it has played. */

#define TAG_STREAM          0x53524450u     /* "PDRS" */
#define STREAM_VTABLE       0x0032DD9Cu

#define STR_VTBL    0
#define STR_TAG     4
#define STR_VOICE   8
#define STR_REFS    12
#define STR_INDEX   16
#define STR_SIZE    32

#define XMEDIAPACKET_STATUS_SUCCESS 0x00000000u
#define XMEDIAPACKET_STATUS_PENDING 0x8000000Au
#define XMEDIAPACKET_STATUS_FLUSHED 0x80004004u
#define XMO_STATUSF_ACCEPT_INPUT_DATA 0x00000001u

#define MAX_STREAMS     32
#define STREAM_PACKETS  64

typedef struct stream_packet {
    uint32_t completed_size, status, event, bytes;
} stream_packet;

typedef struct stream {
    uint32_t      obj;
    int           voice;
    uint32_t      max_packets;
    uint32_t      callback, context;
    uint32_t      block_align;
    uint32_t      next;                 /* next packet slot */
    stream_packet packets[STREAM_PACKETS];
} stream;

static stream streams[MAX_STREAMS];

static stream *stream_of(uint32_t obj)
{
    uint32_t i;

    if (!obj || MEM32(obj + STR_TAG) != TAG_STREAM)
        return NULL;
    i = MEM32(obj + STR_INDEX);
    return i < MAX_STREAMS && streams[i].obj == obj ? &streams[i] : NULL;
}

/* Mixer thread: a packet has been played, or flushed. */
static void packet_done(void *user, uint32_t tag, bool flushed)
{
    stream *s = user;
    stream_packet *p = &s->packets[tag % STREAM_PACKETS];

    if (p->completed_size)
        MEM32(p->completed_size) = flushed ? 0 : p->bytes;
    if (p->status)
        MEM32(p->status) = flushed ? XMEDIAPACKET_STATUS_FLUSHED : XMEDIAPACKET_STATUS_SUCCESS;
}

/* DirectSoundCreateStream(pdssd, ppStream). DSSTREAMDESC: dwFlags,
 * dwMaxAttachedPackets, lpwfxFormat, lpfnCallback, lpvContext, lpMixBins. */
void sub_002C4A34(void)
{
    uint32_t desc = ARG(0), pp = ARG(1), wfx, mixbins, obj;
    pdo_voice_desc vd;
    stream *s = NULL;
    int i;

    if (!desc || !pp)
        RETURN(8, DSERR_INVALIDCALL);
    for (i = 0; i < MAX_STREAMS && !s; i++)
        if (!streams[i].obj)
            s = &streams[i];
    if (!s)
        RETURN(8, DSERR_OUTOFMEMORY);
    if (!the_dsound)
        pdo_mixer_init();

    wfx = MEM32(desc + 8);
    mixbins = MEM32(desc + 20);
    memset(&vd, 0, sizeof vd);
    vd.format = PDO_FMT_PCM16;
    vd.channels = 2;
    vd.sample_rate = 48000;
    if (wfx) {
        uint16_t tag = MEM16(wfx), bits = MEM16(wfx + 14);
        vd.channels = MEM16(wfx + 2) == 1 ? 1 : 2;
        vd.sample_rate = MEM32(wfx + 4);
        if (tag == 0x0069) {
            vd.format = PDO_FMT_XADPCM;
            vd.samples_per_block = MEM16(wfx + 16) >= 2 ? MEM16(wfx + 18) : 64;
        } else {
            vd.format = bits == 8 ? PDO_FMT_PCM8 : PDO_FMT_PCM16;
            if (tag != 1)
                SDL_Log("dsound: stream format 0x%04x, treating as PCM", tag);
        }
    }
    if (mixbins && MEM32(mixbins)) {
        uint32_t n = MEM32(mixbins), pairs = MEM32(mixbins + 4);
        for (uint32_t k = 0; k < n && k < PDO_MAX_VOICE_BINS && pairs; k++) {
            vd.bins[vd.bin_count].bin = MEM32(pairs + k * 8);
            vd.bins[vd.bin_count].volume = (int32_t)MEM32(pairs + k * 8 + 4);
            vd.bin_count++;
        }
    } else {
        vd.bins[0].bin = PDO_BIN_FRONT_LEFT;
        vd.bins[1].bin = PDO_BIN_FRONT_RIGHT;
        vd.bin_count = 2;
    }

    memset(s, 0, sizeof *s);
    s->voice = pdo_voice_create(&vd);
    obj = guest_alloc(STR_SIZE);
    if (!s->voice || !obj)
        RETURN(8, DSERR_OUTOFMEMORY);
    s->obj = obj;
    s->max_packets = MEM32(desc + 4) ? MEM32(desc + 4) : 1;
    if (s->max_packets > STREAM_PACKETS)
        s->max_packets = STREAM_PACKETS;
    s->callback = MEM32(desc + 12);
    s->context = MEM32(desc + 16);
    s->block_align = pdo_voice_block_align(s->voice);
    MEM32(obj + STR_VTBL) = STREAM_VTABLE;
    MEM32(obj + STR_TAG) = TAG_STREAM;
    MEM32(obj + STR_VOICE) = (uint32_t)s->voice;
    MEM32(obj + STR_REFS) = 1;
    MEM32(obj + STR_INDEX) = (uint32_t)(s - streams);
    pdo_voice_set_packet_done(s->voice, packet_done, s);
    pdo_voice_play(s->voice, false);    /* plays packets as they arrive */
    if (s->callback)
        note_once("stream packet callbacks");
    MEM32(pp) = obj;
    SDL_Log("dsound: stream 0x%08x: %s %u ch %u Hz, %u packets", obj,
            vd.format == PDO_FMT_XADPCM ? "adpcm" : vd.format == PDO_FMT_PCM8 ? "pcm8" : "pcm16",
            vd.channels, vd.sample_rate, s->max_packets);
    RETURN(8, DS_OK);
}

/* CDirectSoundStream::AddRef(this) */
void sub_002C1D21(void)
{
    uint32_t o = ARG(0);
    RETURN(4, stream_of(o) ? (MEM32(o + STR_REFS) += 1) : 0);
}

/* CDirectSoundStream::Release(this) */
void sub_002C1D68(void)
{
    uint32_t o = ARG(0), refs = 0;
    stream *s = stream_of(o);

    if (s && MEM32(o + STR_REFS)) {
        refs = (MEM32(o + STR_REFS) -= 1);
        if (!refs) {
            pdo_voice_flush(s->voice);
            pdo_voice_destroy(s->voice);
            MEM32(o + STR_TAG) = 0;
            s->obj = 0;
            xbox_HeapFree(o);
        }
    }
    RETURN(4, refs);
}

/* CDirectSoundStream::GetInfo(this, pInfo): XMEDIAINFO { dwFlags,
 * dwInputSize, dwOutputSize, dwMaxLookahead }. */
void sub_002C1DB6(void)
{
    stream *s = stream_of(ARG(0));
    uint32_t info = ARG(1);

    if (!s || !info)
        RETURN(8, DSERR_INVALIDCALL);
    MEM32(info + 0) = 0x00000005u;      /* XMO_STREAMF_FIXED_SAMPLE_SIZE | INPUT_ASYNC */
    MEM32(info + 4) = s->block_align;
    MEM32(info + 8) = 0;
    MEM32(info + 12) = 0;
    RETURN(8, DS_OK);
}

/* CDirectSoundStream::GetStatus(this, pdwStatus) */
void sub_002C1EB7(void)
{
    stream *s = stream_of(ARG(0));

    if (!s)
        RETURN(8, DSERR_INVALIDCALL);
    if (ARG(1))
        MEM32(ARG(1)) = (uint32_t)pdo_voice_queued(s->voice) < s->max_packets
                      ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0;
    RETURN(8, DS_OK);
}

/* CDirectSoundStream::Process(this, pInputPacket, pOutputPacket).
 * XMEDIAPACKET: pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus,
 * hCompletionEvent (pContext with a callback), prtTimestamp. */
void sub_002C1F08(void)
{
    stream *s = stream_of(ARG(0));
    uint32_t in = ARG(1), slot;
    stream_packet *p;

    if (!s || !in)
        RETURN(12, DSERR_INVALIDCALL);
    if ((uint32_t)pdo_voice_queued(s->voice) >= s->max_packets)
        RETURN(12, DSERR_INVALIDCALL);
    slot = s->next++ % STREAM_PACKETS;
    p = &s->packets[slot];
    p->bytes = MEM32(in + 4);
    p->completed_size = MEM32(in + 8);
    p->status = MEM32(in + 12);
    p->event = MEM32(in + 16);
    if (p->event && !s->callback)
        note_once("stream packet completion events");
    if (p->completed_size)
        MEM32(p->completed_size) = 0;
    if (p->status)
        MEM32(p->status) = XMEDIAPACKET_STATUS_PENDING;
    if (!pdo_voice_queue(s->voice, MEM32(in), p->bytes, slot)) {
        if (p->status)
            MEM32(p->status) = XMEDIAPACKET_STATUS_FLUSHED;
        RETURN(12, DSERR_INVALIDCALL);
    }
    TRACE("stream 0x%08x Process 0x%08x, %u bytes", ARG(0), MEM32(in), p->bytes);
    RETURN(12, DS_OK);
}

/* CDirectSoundStream::Discontinuity(this): the end of the data for now;
 * what is queued still plays. */
void sub_002C1E1D(void)
{
    RETURN(4, stream_of(ARG(0)) ? DS_OK : DSERR_INVALIDCALL);
}

/* CDirectSoundStream::Flush(this) */
void sub_002C1E6A(void)
{
    stream *s = stream_of(ARG(0));
    if (s)
        pdo_voice_flush(s->voice);
    RETURN(4, s ? DS_OK : DSERR_INVALIDCALL);
}

static int stream_voice(uint32_t obj)
{
    stream *s = stream_of(obj);
    return s ? s->voice : 0;
}

/* IDirectSoundStream_* setters (5-byte thunks into CDirectSoundStream, or
 * small wrappers), same arguments as the buffer ones. */
/* SetVolume(pStream, lVolume) */
void sub_002C2682(void)
{
    int id = stream_voice(ARG(0));
    if (id)
        pdo_voice_set_volume(id, (int32_t)ARG(1));
    RETURN(8, DS_OK);
}

/* SetHeadroom(pStream, dwHeadroom) */
void sub_002C2696(void)
{
    int id = stream_voice(ARG(0));
    if (id)
        pdo_voice_set_headroom(id, ARG(1));
    RETURN(8, DS_OK);
}

/* SetFrequency(pStream, dwFrequency) */
void sub_002C39C9(void)
{
    int id = stream_voice(ARG(0));
    if (id)
        pdo_voice_set_frequency(id, ARG(1));
    RETURN(8, DS_OK);
}

/* SetMixBins(pStream, pMixBins) */
void sub_002C269B(void)
{
    int id = stream_voice(ARG(0));
    uint32_t mb = ARG(1);
    pdo_voice_bin bins[PDO_MAX_VOICE_BINS];
    int n = 0;

    if (id && mb) {
        uint32_t count = MEM32(mb), pairs = MEM32(mb + 4);
        for (uint32_t i = 0; i < count && n < PDO_MAX_VOICE_BINS && pairs; i++, n++) {
            bins[n].bin = MEM32(pairs + i * 8);
            bins[n].volume = (int32_t)MEM32(pairs + i * 8 + 4);
        }
        pdo_voice_set_bins(id, bins, n);
    }
    RETURN(8, DS_OK);
}

/* Pause(pStream, dwPause) */
void sub_002C26A0(void)
{
    pause_voice(stream_voice(ARG(0)), ARG(1));
    RETURN(8, DS_OK);
}

void sub_002C2687(void) { note_once("stream LFOs (SetLFO)"); RETURN(8, DS_OK); }
void sub_002C268C(void) { note_once("stream envelopes (SetEG)"); RETURN(8, DS_OK); }
void sub_002C2691(void) { note_once("stream filters (SetFilter)"); RETURN(8, DS_OK); }

/* Stream 3D state, recorded only for now. */
void sub_002C39CE(void) { note_once("3D streams"); RETURN(12, DS_OK); } /* SetAllParameters */
void sub_002C39D3(void) { RETURN(16, DS_OK); }  /* SetConeAngles */
void sub_002C39D8(void) { RETURN(20, DS_OK); }  /* SetConeOrientation */
void sub_002C3A01(void) { RETURN(12, DS_OK); }  /* SetConeOutsideVolume */
void sub_002C3A06(void) { RETURN(12, DS_OK); }  /* SetMaxDistance */
void sub_002C3A1E(void) { RETURN(12, DS_OK); }  /* SetMinDistance */
void sub_002C3A36(void) { RETURN(12, DS_OK); }  /* SetMode */
void sub_002C3A3B(void) { RETURN(20, DS_OK); }  /* SetPosition */
void sub_002C3A64(void) { RETURN(20, DS_OK); }  /* SetVelocity */
void sub_002C3A8D(void) { RETURN(12, DS_OK); }  /* SetDistanceFactor */
void sub_002C3AA5(void) { RETURN(12, DS_OK); }  /* SetDopplerFactor */
void sub_002C3ABD(void) { RETURN(12, DS_OK); }  /* SetRolloffFactor */
void sub_002C3AD5(void) { RETURN(12, DS_OK); }  /* SetI3DL2Source */
