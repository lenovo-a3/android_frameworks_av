/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "SoftwareRenderer"
#include <utils/Log.h>

#include "../include/SoftwareRenderer.h"

#include <cutils/properties.h> // for property_get
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <system/window.h>
#include <ui/GraphicBufferMapper.h>
#include <gui/IGraphicBufferProducer.h>

namespace android {

static bool runningInEmulator() {
    char prop[PROPERTY_VALUE_MAX];
    return (property_get("ro.kernel.qemu", prop, NULL) > 0);
}

#ifdef MTK_HARDWARE
// OMX_COLOR_FormatVendorMTKYUV: what the MT6589 video decoder (MtkOmxVdec)
// writes. NV12 in tiles: the Y plane in 16x32 byte tiles, then (at
// stride * sliceHeight) the interleaved CbCr plane in 16x16 byte tiles, both
// in row-major tile order. Stock shows it through MTK's HWC/MDP; with GLES
// composition we de-tile it into YV12 here.
static const OMX_COLOR_FORMATTYPE kMtkColorFormatYuvBlock =
        (OMX_COLOR_FORMATTYPE)0x7F000001;

// Address of byte (x, y) in a plane of tw x th byte tiles, stride bytes wide.
static inline const uint8_t *mtkTileAddr(
        const uint8_t *src, int stride, int tw, int th, int x, int y) {
    return src + ((y / th) * (stride / tw) + x / tw) * (tw * th)
            + (y % th) * tw + (x % tw);
}

static void mtkDetileYuvBlockToYV12(
        const uint8_t *src, int stride, int sliceHeight,
        int left, int top, int width, int height,
        uint8_t *dstY, uint8_t *dstV, uint8_t *dstU,
        size_t dstYStride, size_t dstCStride) {
    if (left % 16 == 0) {
        // Fast path: walk whole 16-byte tile rows.
        const int tilesPerRow = stride / 16;
        const int cols = (width + 15) / 16;
        for (int y = 0; y < height; ++y) {
            int sy = top + y;
            const uint8_t *s = src + ((sy / 32) * tilesPerRow + left / 16) * 512
                    + (sy % 32) * 16;
            uint8_t *d = dstY + y * dstYStride;
            for (int t = 0; t < cols; ++t) {
                int n = width - t * 16;
                memcpy(d + t * 16, s + t * 512, n < 16 ? n : 16);
            }
        }

        const uint8_t *srcC = src + stride * sliceHeight;
        const int cTop = top / 2;
        const int cWidth = (width + 1) / 2, cHeight = (height + 1) / 2;
        for (int y = 0; y < cHeight; ++y) {
            int sy = cTop + y;
            const uint8_t *s = srcC + ((sy / 16) * tilesPerRow + left / 16) * 256
                    + (sy % 16) * 16;
            uint8_t *du = dstU + y * dstCStride;
            uint8_t *dv = dstV + y * dstCStride;
            for (int x = 0; x < cWidth; x += 8) {
                // 8 Cb,Cr pairs per 16-byte tile row.
                const uint8_t *p = s + (x / 8) * 256;
                int n = cWidth - x < 8 ? cWidth - x : 8;
                for (int i = 0; i < n; ++i) {
                    du[x + i] = p[2 * i];
                    dv[x + i] = p[2 * i + 1];
                }
            }
        }
        return;
    }

    // Generic path (crop not 16-aligned).
    // Y: 16x32 tiles. Copy whole 16-byte tile rows where possible.
    for (int y = 0; y < height; ++y) {
        uint8_t *d = dstY + y * dstYStride;
        int x = 0;
        while (x < width) {
            int sx = left + x;
            int n = 16 - (sx % 16);
            if (n > width - x) {
                n = width - x;
            }
            memcpy(d + x, mtkTileAddr(src, stride, 16, 32, sx, top + y), n);
            x += n;
        }
    }

    // CbCr: 16x16 tiles of interleaved Cb,Cr pairs.
    const uint8_t *srcC = src + stride * sliceHeight;
    int cTop = top / 2, cLeft = (left / 2) * 2;
    int cWidth = (width + 1) / 2, cHeight = (height + 1) / 2;
    for (int y = 0; y < cHeight; ++y) {
        uint8_t *du = dstU + y * dstCStride;
        uint8_t *dv = dstV + y * dstCStride;
        for (int x = 0; x < cWidth; ++x) {
            const uint8_t *p = mtkTileAddr(
                    srcC, stride, 16, 16, cLeft + 2 * x, cTop + y);
            du[x] = p[0];
            dv[x] = p[1];
        }
    }
}
#endif

static int ALIGN(int x, int y) {
    // y must be a power of 2.
    return (x + y - 1) & ~(y - 1);
}

SoftwareRenderer::SoftwareRenderer(const sp<ANativeWindow> &nativeWindow)
    : mColorFormat(OMX_COLOR_FormatUnused),
      mConverter(NULL),
      mYUVMode(None),
      mNativeWindow(nativeWindow),
      mWidth(0),
      mHeight(0),
      mCropLeft(0),
      mCropTop(0),
      mCropRight(0),
      mCropBottom(0),
      mCropWidth(0),
      mCropHeight(0) {
}

SoftwareRenderer::~SoftwareRenderer() {
    delete mConverter;
    mConverter = NULL;
}

void SoftwareRenderer::resetFormatIfChanged(const sp<AMessage> &format) {
    CHECK(format != NULL);

    int32_t colorFormatNew;
    CHECK(format->findInt32("color-format", &colorFormatNew));

    int32_t widthNew, heightNew;
    CHECK(format->findInt32("stride", &widthNew));
    CHECK(format->findInt32("slice-height", &heightNew));

    int32_t cropLeftNew, cropTopNew, cropRightNew, cropBottomNew;
    if (!format->findRect(
            "crop", &cropLeftNew, &cropTopNew, &cropRightNew, &cropBottomNew)) {
        cropLeftNew = cropTopNew = 0;
        cropRightNew = widthNew - 1;
        cropBottomNew = heightNew - 1;
    }

    if (static_cast<int32_t>(mColorFormat) == colorFormatNew &&
        mWidth == widthNew &&
        mHeight == heightNew &&
        mCropLeft == cropLeftNew &&
        mCropTop == cropTopNew &&
        mCropRight == cropRightNew &&
        mCropBottom == cropBottomNew) {
        // Nothing changed, no need to reset renderer.
        return;
    }

    mColorFormat = static_cast<OMX_COLOR_FORMATTYPE>(colorFormatNew);
    mWidth = widthNew;
    mHeight = heightNew;
    mCropLeft = cropLeftNew;
    mCropTop = cropTopNew;
    mCropRight = cropRightNew;
    mCropBottom = cropBottomNew;

    mCropWidth = mCropRight - mCropLeft + 1;
    mCropHeight = mCropBottom - mCropTop + 1;

    int halFormat;
    size_t bufWidth, bufHeight;

    switch (mColorFormat) {
#ifdef MTK_HARDWARE
        case kMtkColorFormatYuvBlock:
#endif
        case OMX_COLOR_FormatYUV420Planar:
        case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar:
        case OMX_COLOR_FormatYUV420SemiPlanar:
        {
            if (!runningInEmulator()) {
                halFormat = HAL_PIXEL_FORMAT_YV12;
                bufWidth = (mCropWidth + 1) & ~1;
                bufHeight = (mCropHeight + 1) & ~1;
                break;
            }

            // fall through.
        }

        default:
            halFormat = HAL_PIXEL_FORMAT_RGB_565;
            bufWidth = mCropWidth;
            bufHeight = mCropHeight;

            mConverter = new ColorConverter(
                    mColorFormat, OMX_COLOR_Format16bitRGB565);
            CHECK(mConverter->isValid());
            break;
    }

    CHECK(mNativeWindow != NULL);
    CHECK(mCropWidth > 0);
    CHECK(mCropHeight > 0);
    CHECK(mConverter == NULL || mConverter->isValid());

#ifdef EXYNOS4_ENHANCEMENTS
    CHECK_EQ(0,
            native_window_set_usage(
            mNativeWindow.get(),
            GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_OFTEN
            | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP
            | GRALLOC_USAGE_HW_FIMC1));
#else
    CHECK_EQ(0,
            native_window_set_usage(
            mNativeWindow.get(),
            GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_OFTEN
            | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP));
#endif

    CHECK_EQ(0,
            native_window_set_scaling_mode(
            mNativeWindow.get(),
            NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW));

    // Width must be multiple of 32???
    CHECK_EQ(0, native_window_set_buffers_dimensions(
                mNativeWindow.get(),
                bufWidth,
                bufHeight));
    CHECK_EQ(0, native_window_set_buffers_format(
                mNativeWindow.get(),
                halFormat));

    // NOTE: native window uses extended right-bottom coordinate
    android_native_rect_t crop;
    crop.left = mCropLeft;
    crop.top = mCropTop;
    crop.right = mCropRight + 1;
    crop.bottom = mCropBottom + 1;
    ALOGV("setting crop: [%d, %d, %d, %d] for size [%zu, %zu]",
          crop.left, crop.top, crop.right, crop.bottom, bufWidth, bufHeight);

    CHECK_EQ(0, native_window_set_crop(mNativeWindow.get(), &crop));

    int32_t rotationDegrees;
    if (!format->findInt32("rotation-degrees", &rotationDegrees)) {
        rotationDegrees = 0;
    }
    uint32_t transform;
    switch (rotationDegrees) {
        case 0: transform = 0; break;
        case 90: transform = HAL_TRANSFORM_ROT_90; break;
        case 180: transform = HAL_TRANSFORM_ROT_180; break;
        case 270: transform = HAL_TRANSFORM_ROT_270; break;
        default: transform = 0; break;
    }

    CHECK_EQ(0, native_window_set_buffers_transform(
                mNativeWindow.get(), transform));
}

void SoftwareRenderer::render(
        const void *data, size_t size, int64_t timestampNs,
        void* /*platformPrivate*/, const sp<AMessage>& format) {
    resetFormatIfChanged(format);

    ANativeWindowBuffer *buf;
    int err;
    if ((err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(),
            &buf)) != 0) {
        ALOGW("Surface::dequeueBuffer returned error %d", err);
        return;
    }

    GraphicBufferMapper &mapper = GraphicBufferMapper::get();

    Rect bounds(mCropWidth, mCropHeight);

    void *dst;
    CHECK_EQ(0, mapper.lock(
                buf->handle, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &dst));

    if (mConverter) {
        mConverter->convert(
                data,
                mWidth, mHeight,
                mCropLeft, mCropTop, mCropRight, mCropBottom,
                dst,
                buf->stride, buf->height,
                0, 0, mCropWidth - 1, mCropHeight - 1);
#ifdef MTK_HARDWARE
    } else if (mColorFormat == kMtkColorFormatYuvBlock) {
        if ((size_t)mWidth * mHeight * 3 / 2 > size
                || mWidth % 16 || mHeight % 32) {
            ALOGW("unexpected MTK block buffer: %dx%d, %zu bytes",
                    mWidth, mHeight, size);
            goto skip_copying;
        }
        size_t dst_y_size = buf->stride * buf->height;
        size_t dst_c_stride = ALIGN(buf->stride / 2, 16);
        size_t dst_c_size = dst_c_stride * buf->height / 2;
        uint8_t *dst_y = (uint8_t *)dst;
        uint8_t *dst_v = dst_y + dst_y_size;
        uint8_t *dst_u = dst_v + dst_c_size;

        mtkDetileYuvBlockToYV12(
                (const uint8_t *)data, mWidth, mHeight,
                mCropLeft, mCropTop, mCropWidth, mCropHeight,
                dst_y, dst_v, dst_u, buf->stride, dst_c_stride);
#endif
    } else if (mColorFormat == OMX_COLOR_FormatYUV420Planar) {
        if ((size_t)mWidth * mHeight * 3 / 2 > size) {
            goto skip_copying;
        }
        const uint8_t *src_y = (const uint8_t *)data;
        const uint8_t *src_u = (const uint8_t *)data + mWidth * mHeight;
        const uint8_t *src_v = src_u + (mWidth / 2 * mHeight / 2);

        uint8_t *dst_y = (uint8_t *)dst;
        size_t dst_y_size = buf->stride * buf->height;

#ifdef EXYNOS4_ENHANCEMENTS
        size_t dst_c_stride = buf->stride / 2;
        size_t dst_c_size = dst_c_stride * buf->height / 2;
        size_t dst_c_size_aligned = ALIGN(buf->stride / 2, 16) * buf->height / 2;
        uint8_t *dst_v = dst_y + dst_y_size;
        uint8_t *dst_u = dst_v + dst_c_size_aligned;
#else
        size_t dst_c_stride = ALIGN(buf->stride / 2, 16);
        size_t dst_c_size = dst_c_stride * buf->height / 2;
        uint8_t *dst_v = dst_y + dst_y_size;
        uint8_t *dst_u = dst_v + dst_c_size;
#endif

        for (int y = 0; y < mCropHeight; ++y) {
            memcpy(dst_y, src_y, mCropWidth);

            src_y += mWidth;
            dst_y += buf->stride;
        }

        for (int y = 0; y < (mCropHeight + 1) / 2; ++y) {
            memcpy(dst_u, src_u, (mCropWidth + 1) / 2);
            memcpy(dst_v, src_v, (mCropWidth + 1) / 2);

            src_u += mWidth / 2;
            src_v += mWidth / 2;
            dst_u += dst_c_stride;
            dst_v += dst_c_stride;
        }
    } else if (mColorFormat == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar
            || mColorFormat == OMX_COLOR_FormatYUV420SemiPlanar) {
        if ((size_t)mWidth * mHeight * 3 / 2 > size) {
            goto skip_copying;
        }

        const uint8_t *src_y =
            (const uint8_t *)data;

        const uint8_t *src_uv =
            (const uint8_t *)data + mWidth * (mHeight - mCropTop / 2);

#ifdef EXYNOS4_ENHANCEMENTS
        void *pYUVBuf[3];

        CHECK_EQ(0, mapper.unlock(buf->handle));
        CHECK_EQ(0, mapper.lock(
                buf->handle, GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_YUV_ADDR, bounds, pYUVBuf));

        size_t dst_c_stride = buf->stride / 2;
        uint8_t *dst_y = (uint8_t *)pYUVBuf[0];
        uint8_t *dst_v = (uint8_t *)pYUVBuf[1];
        uint8_t *dst_u = (uint8_t *)pYUVBuf[2];
#else
        size_t dst_y_size = buf->stride * buf->height;
        size_t dst_c_stride = ALIGN(buf->stride / 2, 16);
        size_t dst_c_size = dst_c_stride * buf->height / 2;
        uint8_t *dst_y = (uint8_t *)dst;
        uint8_t *dst_v = dst_y + dst_y_size;
        uint8_t *dst_u = dst_v + dst_c_size;
#endif

        for (int y = 0; y < mCropHeight; ++y) {
            memcpy(dst_y, src_y, mCropWidth);

            src_y += mWidth;
            dst_y += buf->stride;
        }

        for (int y = 0; y < (mCropHeight + 1) / 2; ++y) {
            size_t tmp = (mCropWidth + 1) / 2;
            for (size_t x = 0; x < tmp; ++x) {
                dst_u[x] = src_uv[2 * x];
                dst_v[x] = src_uv[2 * x + 1];
            }

            src_uv += mWidth;
            dst_u += dst_c_stride;
            dst_v += dst_c_stride;
        }
    } else {
        LOG_ALWAYS_FATAL("bad color format %#x", mColorFormat);
    }

skip_copying:
    CHECK_EQ(0, mapper.unlock(buf->handle));

    if ((err = native_window_set_buffers_timestamp(mNativeWindow.get(),
            timestampNs)) != 0) {
        ALOGW("Surface::set_buffers_timestamp returned error %d", err);
    }

    if ((err = mNativeWindow->queueBuffer(mNativeWindow.get(), buf,
            -1)) != 0) {
        ALOGW("Surface::queueBuffer returned error %d", err);
    }
    buf = NULL;
}

}  // namespace android
