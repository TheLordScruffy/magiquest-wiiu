// Ctrl_Movie.cpp
//   Written by Palapeli
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Ctrl_Movie.hpp"
#include "AudioMgr.hpp"
#include "yuv2rgb.h"
#include <algorithm>
#include <cassert>

/**
 * Blank image for GuiImageData
 */
static const u8 blankImg[] = {
  0x00,
  0x00,
  0x02,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x01,
  0x00,
  0x01,
  0x00,
  0x20,
  0x28,
  0xFF,
  0xFF,
  0xFF,
  0x00,
};

Ctrl_Movie::Ctrl_Movie()
  : m_image(nullptr)
  , m_imageData(blankImg, sizeof(blankImg), GX2_TEX_CLAMP_MODE_CLAMP,
      GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8)
  , m_decoder(this, 100, 40, 768, 768)
{
    OSInitMessageQueueEx(
      &m_fbInQueue, m_fbInMsg, FrameBufferCount, "Ctrl_Movie::m_fbInQueue");
    OSInitMessageQueueEx(
      &m_fbOutQueue, m_fbOutMsg, FrameBufferCount, "Ctrl_Movie::m_fbOutQueue");
    OSInitMessageQueueEx(&m_ctrlQueue, m_ctrlMsg, 8, "Ctrl_Movie::m_ctrlQueue");
    OSInitMessageQueueEx(
      &m_audioCtrlQueue, m_audioCtrlMsg, 8, "Ctrl_Movie::m_audioCtrlQueue");

    OSInitMessageQueueEx(&m_voiceLReturnQueue, m_voiceLReturnMsg, 8,
      "Ctrl_Movie::m_voiceLReturnQueue");
    OSInitMessageQueueEx(&m_voiceRReturnQueue, m_voiceRReturnMsg, 8,
      "Ctrl_Movie::m_voiceRReturnQueue");

    OSInitMutexEx(&m_fileMutex, "Ctrl_Movie::m_fileMutex");

    m_image.setColorIntensity({1.1, 1.1, 1.1, 1});

    // Initialize a texture to get the framebuffer size
    GX2Texture tempTex;
    GX2InitTexture(&tempTex, MaxWidth, MaxHeight, 1, 1,
      GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_SURFACE_DIM_TEXTURE_2D,
      GX2_TILE_MODE_LINEAR_ALIGNED);
    assert(tempTex.surface.imageSize != 0);

    m_fbData = std::unique_ptr<u8>(new (std::align_val_t(256))
        u8[tempTex.surface.imageSize * FrameBufferCount]);

    for (u32 i = 0; i < FrameBufferCount; i++) {
        OSMessage msg = {
          .message = m_fbData.get() + i * tempTex.surface.imageSize,
          .args = {0},
        };
        auto ret = OSSendMessage(&m_fbInQueue, &msg, OS_MESSAGE_FLAGS_NONE);
        assert(ret);
    }

    // Workaround, GuiImageData doesn't usually allow us to do anything like
    // this. It allocates a GX2Texture when we supply the blank image and then
    // we const_cast our way into accessing it.
    auto tex = const_cast<GX2Texture*>(m_imageData.getTexture());
    assert(tex != nullptr);

    m_savedImageData = tex->surface.image;
    tex->surface.image = nullptr;

    m_rgbFrameSize = 0;

    auto ax = AudioMgr::s_instance;

    m_voiceL = ax->AcquireVoice(
      0x40000000, true, false, true, false, 48000, true, false);
    assert(m_voiceL != -1);

    m_voiceR = ax->AcquireVoice(
      0x40000000, false, true, false, true, 48000, true, false);
    assert(m_voiceR != -1);

    ax->SetBufferOutQueue(m_voiceL, &m_voiceLReturnQueue);
    ax->SetBufferOutQueue(m_voiceR, &m_voiceRReturnQueue);

    // Create a large audio buffer and then split it up into multiple parts for
    // each voice queue.
    m_audioBufferSize = 32000;
    const u32 audioBufferCount = 4;

    m_audioData =
      std::unique_ptr<u16>(new u16[(m_audioBufferSize * audioBufferCount) * 2]);

    for (u32 i = 0; i < audioBufferCount; i++) {
        OSMessage msg = {
          .message = (void*) (m_audioData.get() + m_audioBufferSize * i),
          .args =
            {
              m_audioBufferSize,
            },
        };

        auto ret =
          OSSendMessage(&m_voiceLReturnQueue, &msg, OS_MESSAGE_FLAGS_NONE);
        assert(ret);
    }

    for (u32 i = audioBufferCount; i < audioBufferCount * 2; i++) {
        OSMessage msg = {
          .message = (void*) (m_audioData.get() + m_audioBufferSize * i),
          .args =
            {
              m_audioBufferSize,
            },
        };

        auto ret =
          OSSendMessage(&m_voiceRReturnQueue, &msg, OS_MESSAGE_FLAGS_NONE);
        assert(ret);
    }

    m_videoThread.Run([&]() { VideoDecodeProc(); });
    m_audioThread.Run([&]() { AudioDecodeProc(); });
}

Ctrl_Movie::Ctrl_Movie(const char* path)
  : Ctrl_Movie()
{
    ChangeMovie(path);
}

Ctrl_Movie::~Ctrl_Movie()
{
    // Restore the original data so GuiImageData can free it
    auto tex = const_cast<GX2Texture*>(m_imageData.getTexture());
    if (tex != nullptr) {
        tex->surface.image = m_savedImageData;
    }
}

void Ctrl_Movie::process()
{
    static int frameThing = 0;
    frameThing ^= 1;

    OSMessage msg = {};

    if (frameThing != 0)
        return;

    if (!OSReceiveMessage(&m_fbOutQueue, &msg, OS_MESSAGE_FLAGS_NONE)) {
        LOG(LogMP4, "Missed frame");
        return;
    }

    u32 width = msg.args[0];
    u32 height = msg.args[1];
    u32 frameId = msg.args[2];

    if (frameId == 0) {
        static int marker = 1;
        AudioMgr::s_instance->ChangeMarker(m_voiceL, marker);
        AudioMgr::s_instance->ChangeMarker(m_voiceR, marker);
        marker++;
    }

    auto tex = const_cast<GX2Texture*>(m_imageData.getTexture());
    assert(tex != nullptr);

    if (tex->surface.image != nullptr) {
        OSMessage msg2 = {
          .message = tex->surface.image,
          .args = {},
        };

        auto ret =
          OSSendMessage(&m_fbInQueue, &msg2, OS_MESSAGE_FLAGS_BLOCKING);
        assert(ret);
    }

    GX2InitTexture(tex, width, height, 1, 1,
      GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_SURFACE_DIM_TEXTURE_2D,
      GX2_TILE_MODE_LINEAR_ALIGNED);
    assert(tex->surface.imageSize != 0);

    tex->surface.image = msg.message;

    GX2Invalidate(
      GX2_INVALIDATE_MODE_CPU_TEXTURE, msg.message, tex->surface.imageSize);

    m_image.setImageData(&m_imageData);
    m_image.setScaleX(1440.0 / width);
    m_image.setScaleY(1080.0 / height);
}

void Ctrl_Movie::draw(CVideo* video)
{
    m_image.draw(video);
}

void Ctrl_Movie::VideoDecodeProc()
{
    LOG(LogMP4, "Enter video decode");

    OSMessage fbMsg = {};
    OSMessage ctrlMsg = {};
    u32 it = 0;

    m_streamEnded = true;
    bool noStream = true;

    while (true) {
        if (fbMsg.message == nullptr) {
            // Wait on the framebuffer in queue
            auto ret =
              OSReceiveMessage(&m_fbInQueue, &fbMsg, OS_MESSAGE_FLAGS_BLOCKING);
            assert(ret);
        }

        assert(fbMsg.message != nullptr);

        // Check for a control message before decoding. If the stream has ended
        // then this is blocking
        if (OSReceiveMessage(&m_ctrlQueue, &ctrlMsg,
              noStream ? OS_MESSAGE_FLAGS_BLOCKING : OS_MESSAGE_FLAGS_NONE)) {
            LOG_VERBOSE(LogMP4, "Received control command");

            switch (CtrlCmd(ctrlMsg.args[0])) {
            case CtrlCmd::ChangeMovie:
                LOG(LogMP4, "Changing movie");
                m_decoder.OpenMovie((FILE*) ctrlMsg.args[1]);
                it = 0;
                m_streamEnded = false;
                noStream = false;
                break;

            default:
                assert(!"Received invalid control command");
            }
        }

        u32 width, height;
        if (m_streamEnded ||
            !m_decoder.DecodeFrame(fbMsg.message, &width, &height)) {
            LOG(LogMP4, "End of stream reached");
            if (m_onEndHandler != nullptr) {
                m_onEndHandler(this);
            }
            m_streamEnded = true;
            noStream = true;
            continue;
        }

        fbMsg.args[0] = width;
        fbMsg.args[1] = height;
        fbMsg.args[2] = it++;

        auto ret =
          OSSendMessage(&m_fbOutQueue, &fbMsg, OS_MESSAGE_FLAGS_BLOCKING);
        assert(ret);
        fbMsg = {};
    }
}

void Ctrl_Movie::AudioDecodeProc()
{
    LOG(LogMP4, "Enter audio decode");

    OSMessage vLMsg = {};
    OSMessage vRMsg = {};
    OSMessage ctrlMsg = {};
    u32 marker = 0;

    bool noAudioStream = true;

    while (true) {
        if (vLMsg.message == nullptr) {
            OSReceiveMessage(
              &m_voiceLReturnQueue, &vLMsg, OS_MESSAGE_FLAGS_BLOCKING);
        }

        if (vRMsg.message == nullptr) {
            OSReceiveMessage(
              &m_voiceRReturnQueue, &vRMsg, OS_MESSAGE_FLAGS_BLOCKING);
        }

        // Check for a control message before decoding. If the stream has ended
        // then this is blocking
        if (OSReceiveMessage(&m_audioCtrlQueue, &ctrlMsg,
              noAudioStream ? OS_MESSAGE_FLAGS_BLOCKING
                            : OS_MESSAGE_FLAGS_NONE)) {
            LOG_VERBOSE(LogMP4, "Received control command");

            switch (AudioCtrlCmd(ctrlMsg.args[0])) {
            case AudioCtrlCmd::ChangeAudio:
                LOG(LogMP4, "Changing audio");
                m_decoder.OpenAudio((FILE*) ctrlMsg.args[1]);
                marker++;
                noAudioStream = false;
                break;

            default:
                assert(!"Received invalid control command");
            }
        }

        if (vLMsg.message != nullptr && vRMsg.message != nullptr &&
            !noAudioStream) {
            u32 sampleCount = m_decoder.DecodeAudio((u16*) vLMsg.message,
              (u16*) vRMsg.message, m_audioBufferSize, &noAudioStream);

            if (sampleCount != 0) {
                AudioMgr::s_instance->PushBuffer(
                  m_voiceL, (u16*) vLMsg.message, sampleCount, marker);
                AudioMgr::s_instance->PushBuffer(
                  m_voiceR, (u16*) vRMsg.message, sampleCount, marker);

                vLMsg = {};
                vRMsg = {};
            }
        }
    }
}

bool Ctrl_Movie::ChangeMovie(const char* path)
{
    Lock l(m_fileMutex);

    auto movieFile = fopen(path, "rb");
    if (movieFile == nullptr) {
        LOG(LogMP4, "Failed to open \"%s\"", path);
        return false;
    }

    char sndPath[256];
    snprintf(sndPath, 256, "%s.ogg", path);

    auto audioFile = fopen(sndPath, "rb");
    if (audioFile == nullptr) {
        LOG(LogMP4, "Failed to open \"%s\"", sndPath);
        fclose(movieFile);
        return false;
    }

    OSMessage msg = {
      .message = nullptr,
      .args =
        {
          [0] = u32(CtrlCmd::ChangeMovie),
          [1] = u32(movieFile),
        },
    };

    auto ret = OSSendMessage(&m_ctrlQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
    assert(ret);

    msg = {
      .message = nullptr,
      .args =
        {
          [0] = u32(AudioCtrlCmd::ChangeAudio),
          [1] = u32(audioFile),
        },
    };

    ret = OSSendMessage(&m_audioCtrlQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
    assert(ret);

    return true;
}

void Ctrl_Movie::ForceEndStream()
{
    m_streamEnded = true;
}

void Ctrl_Movie::SetOnEndHandler(std::function<void(Ctrl_Movie*)> handler)
{
    m_onEndHandler = handler;
}

Ctrl_Movie::Decoder::Decoder(
  Ctrl_Movie* parent, u32 profile, u32 level, u32 maxWidth, u32 maxHeight)
  : m_parent(parent)
  , m_file(nullptr)
{
    OSInitMessageQueueEx(
      &m_respQueue, m_respMsg, 4, "Ctrl_Movie::Decoder::m_respQueue");

    // Invalidate the cache by setting it to an impossible offset
    m_cacheOffset = 1;

    u32 memSize;
    auto h264err =
      H264DECMemoryRequirement(profile, level, maxWidth, maxHeight, &memSize);
    assert(h264err == H264_ERROR_OK);

    m_memory = std::make_unique<u8[]>(memSize);
    m_nv12Fb = std::unique_ptr<u8>(new (std::align_val_t(256))
        u8[maxWidth * maxHeight +
           2 * ((maxWidth + 1) / 2) * ((maxHeight + 1) / 2)]);

    h264err = H264DECCheckMemSegmentation(m_memory.get(), memSize);
    assert(h264err == H264_ERROR_OK);

    h264err = H264DECInitParam(memSize, m_memory.get());
    assert(h264err == H264_ERROR_OK);

    h264err = H264DECSetParam_FPTR_OUTPUT(m_memory.get(), H264FrameCallback);
    assert(h264err == H264_ERROR_OK);

    h264err = H264DECSetParam_OUTPUT_PER_FRAME(m_memory.get(), 1);
    assert(h264err == H264_ERROR_OK);

    h264err = H264DECSetParam(m_memory.get(), H264_PARAMETER_USER_MEMORY,
      reinterpret_cast<void*>(this));
    assert(h264err == H264_ERROR_OK);

    m_audioBufferSize = 0x1000;
    m_audioBuffer = std::unique_ptr<u16>(new u16[m_audioBufferSize]);
}

Ctrl_Movie::Decoder::~Decoder()
{
}

const ov_callbacks Ctrl_Movie::Decoder::s_oggCallbacks = {
  OGGRead,
  OGGSeek,
  OGGClose,
  OGGTell,
};

bool Ctrl_Movie::Decoder::OpenMovie(FILE* movieFile)
{
    assert(movieFile != nullptr);

    if (m_file != nullptr) {
        CloseMovie();
    }

    m_sample = 0;

    // XXX Getting the size could probably be done better (and faster) with
    // fstat

    Lock l(m_parent->m_fileMutex);

    if (fseek(movieFile, 0, SEEK_END) != 0) {
        LOG(LogMP4, "Failed to seek to the end of the file");
        return false;
    }

    m_fileSize = ftell(movieFile);

    if (fseek(movieFile, 0, SEEK_SET) != 0) {
        LOG(LogMP4, "Failed to seek to the beginning of the file");
        return false;
    }

    m_file = movieFile;
    m_mp4 = {};
    if (MP4D_open(&m_mp4, MP4ReadCallback, reinterpret_cast<void*>(this),
          m_fileSize) != 1) {
        LOG(LogMP4, "Failed to parse MP4");
        return false;
    }

    m_tr = &m_mp4.track[0];

    auto h264err = H264DECOpen(m_memory.get());
    assert(h264err == H264_ERROR_OK);

    h264err = H264DECBegin(m_memory.get());
    assert(h264err == H264_ERROR_OK);

    LOG(LogMP4, "Movie file opened successfully");
    return true;
}

bool Ctrl_Movie::Decoder::OpenAudio(FILE* audioFile)
{
    assert(audioFile != nullptr);

    ov_clear(&m_oggFile);

    m_audioFile = audioFile;

    auto oggRet =
      ov_open_callbacks(this, &m_oggFile, nullptr, 0, s_oggCallbacks);
    assert(oggRet >= 0);

    auto oggInfo = ov_info(&m_oggFile, -1);
    assert(oggInfo != nullptr);

    LOG(LogMP4, "Audio file opened successfully");
    return true;
}

void Ctrl_Movie::Decoder::CloseMovie()
{
    if (m_file == nullptr)
        return;

    LOG(LogMP4, "Closing file");
    auto f2 = m_file;
    m_file = nullptr;
    m_tr = nullptr;

    Lock l(m_parent->m_fileMutex);

    fclose(f2);
    MP4D_close(&m_mp4);

    auto h264err = H264DECEnd(m_memory.get());
    assert(h264err == H264_ERROR_OK);

    h264err = H264DECClose(m_memory.get());
    assert(h264err == H264_ERROR_OK);
}

bool Ctrl_Movie::Decoder::Read(u8* out, u32 offset, u32 len)
{
    if (m_file == nullptr)
        return false;

    if (offset + len > m_fileSize || offset + len < offset)
        return false;

    u32 offsetRounded = RoundDown(offset, MaxCacheSize);
    u32 offsetInBlock = offset - offsetRounded;
    u32 lengthInBlock = 0;

    auto copyFromCached = [&]() -> bool {
        if (offsetInBlock > m_cacheSize) {
            LOG(LogMP4, "Read off the end of the cache block");
            return false;
        }

        lengthInBlock = std::min<u32>(m_cacheSize - offsetInBlock, len);
        memcpy(out, m_cache + offsetInBlock, lengthInBlock);

        out += lengthInBlock;
        len -= lengthInBlock;
        offset += lengthInBlock;
        offsetRounded = offset;
        offsetInBlock = 0;
        return true;
    };

    if (offsetRounded == m_cacheOffset) {
        // Read from cached block
        if (!copyFromCached())
            return false;
    }

    Lock l(m_parent->m_fileMutex);

    // Read the rest of the data
    while (len > 0) {
        if (m_seekOffset != offsetRounded) {
            if (fseek(m_file, offsetRounded, SEEK_SET) != 0)
                return false;
            m_seekOffset = offsetRounded;
            m_cacheOffset = offsetRounded;
        }

        auto ret = fread(m_cache, 1, MaxCacheSize, m_file);
        if (ret < 1) {
            LOG(LogMP4, "Failed to read");
            return false;
        }

        m_cacheSize = ret;
        m_seekOffset += m_cacheSize;

        if (!copyFromCached())
            return false;
    }

    return true;
}

int Ctrl_Movie::Decoder::MP4ReadCallback(
  s64 offset, void* buffer, size_t size, void* token)
{
    auto that = reinterpret_cast<Ctrl_Movie::Decoder*>(token);
    return that->Read(reinterpret_cast<u8*>(buffer), offset, size) ? 0 : 1;
}

void Ctrl_Movie::Decoder::H264FrameCallback(H264DecodeOutput* output)
{
    assert(output != nullptr);

    OSMessage msg = {};
    auto obj = reinterpret_cast<Decoder*>(output->userMemory);
    assert(obj != nullptr);

    if (output->frameCount == 1) {
        msg.args[0] = 1;
        msg.args[1] = output->decodeResults[0]->width;
        msg.args[2] = output->decodeResults[0]->height;
    }

    auto ret =
      OSSendMessage(&obj->m_respQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
    assert(ret);
}

bool Ctrl_Movie::Decoder::DecodeFrame(
  void* outFb, u32* outWidth, u32* outHeight)
{
    if (m_tr == nullptr || m_file == nullptr)
        return false;

    if (m_sample >= m_tr->sample_count)
        return false;

    u32 byteCount, timestamp, duration;
    auto offset =
      MP4D_frame_offset(&m_mp4, 0, m_sample, &byteCount, &timestamp, &duration);

    if (offset == 0 || byteCount == 0)
        return false;

    m_sampleData = std::make_unique<u8[]>(byteCount);
    if (!Read(m_sampleData.get(), offset, byteCount)) {
        LOG(LogMP4, "Failed to read from stream");
        return false;
    }

    auto bitstream = m_sampleData.get();

    for (u32 tempOffset = 0; tempOffset < byteCount;) {
        u32 size = (*(u32*) (bitstream + tempOffset)) + 4;
        if (size > byteCount || size < 5) {
            LOG(LogMP4, "Invalid frame size: %08X", size);
            break;
        }

        // Change the size field into the H.264 sync data
        bitstream[tempOffset] = 0;
        bitstream[tempOffset + 1] = 0;
        bitstream[tempOffset + 2] = 0;
        bitstream[tempOffset + 3] = 1;
        tempOffset += size;
    }

    // XXX Should check if the frame can be decoded before attempting to
    // decode

    auto h264err = H264DECSetBitstream(
      m_memory.get(), bitstream, byteCount, double(timestamp) / 1000000);
    if (h264err != H264_ERROR_OK) {
        LOG(LogMP4, "H264DECSetBitstream error: %X", h264err);
        return false;
    }

    h264err = H264DECExecute(m_memory.get(), m_nv12Fb.get());
    if ((h264err & 0xFFFFFF00) != H264_ERROR_OK) {
        LOG(LogMP4, "H264DECExecute error: %X", h264err);
        return false;
    }

#if 0
        h264err = H264DECFlush(m_memory.get());
        if (h264err != H264_ERROR_OK) {
            LOG(LogMP4, "H264DECFlush error: %X", h264err);
            // I'm not exactly sure what we do in this situation, so just...
            // ignore it?
        }
#endif

    OSMessage msg;
    if (!OSReceiveMessage(&m_respQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING)) {
        LOG(LogMP4, "Failed to receive response from the queue");
        return false;
    }

    m_sample++;

    if (msg.args[0] != 1)
        return false;

    u32 width = msg.args[1];
    u32 height = msg.args[2];

    u8* y = m_nv12Fb.get();
    u8* uv = y + RoundUp(width, 256) * height;
    u32 yuvStride = RoundUp(width, 256);

    // Convert the NV12 image to RGBA
    nv12_rgba32_std(width, height, y, uv, yuvStride, yuvStride,
      reinterpret_cast<u8*>(outFb), RoundUp(width, 256) * 4, 1);

    *outWidth = width;
    *outHeight = height;
    return true;
}

size_t Ctrl_Movie::Decoder::OGGRead(
  void* punt, size_t bytes, size_t blocks, void* f)
{
    assert(f != nullptr);
    auto obj = reinterpret_cast<Decoder*>(f);

    Lock l(obj->m_parent->m_fileMutex);

    return fread(punt, 1, bytes * blocks, obj->m_audioFile);
}

int Ctrl_Movie::Decoder::OGGSeek(void* f, ogg_int64_t offset, int mode)
{
    assert(f != nullptr);
    auto obj = reinterpret_cast<Decoder*>(f);

    Lock l(obj->m_parent->m_fileMutex);

    return fseek(obj->m_audioFile, offset, mode);
}

int Ctrl_Movie::Decoder::OGGClose(void* f)
{
    assert(f != nullptr);
    auto obj = reinterpret_cast<Decoder*>(f);

    Lock l(obj->m_parent->m_fileMutex);

    fclose(obj->m_audioFile);
    return 0;
}

long Ctrl_Movie::Decoder::OGGTell(void* f)
{
    assert(f != nullptr);
    auto obj = reinterpret_cast<Decoder*>(f);

    Lock l(obj->m_parent->m_fileMutex);

    return ftell(obj->m_audioFile);
}

u32 Ctrl_Movie::Decoder::DecodeAudio(
  u16* bufferL, u16* bufferR, u32 sampleCount, bool* end)
{
    int bitstream = 0;

    u32 read = 0;
    int signedSampleCount = sampleCount;

    while (signedSampleCount > 0) {
        const u32 amountToRead = std::min<u32>(
          m_audioBufferSize * sizeof(u16), signedSampleCount * sizeof(u16) * 2);

        auto ret = ov_read(
          &m_oggFile, (char*) m_audioBuffer.get(), amountToRead, &bitstream);

        if (ret <= 0) {
            LOG(LogMP4, "Reached end of audio stream");
            assert(end != nullptr);
            *end = true;
            return read;
        }

        for (int i = 0; i < ret / 4; i++) {
            bufferL[i + read] = m_audioBuffer.get()[i << 1];
        }

        for (int i = 0; i < ret / 4; i++) {
            bufferR[i + read] = m_audioBuffer.get()[(i << 1) + 1];
        }

        read += ret / 4;
        signedSampleCount -= ret / 4;
    }

    *end = false;
    return read;
}
