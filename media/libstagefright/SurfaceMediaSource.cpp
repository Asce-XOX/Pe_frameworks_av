/*
 * Copyright (C) 2011 The Android Open Source Project
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
//#define LOG_NDEBUG 0
#define LOG_TAG "SurfaceMediaSource"

#include <inttypes.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/SurfaceMediaSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <OMX_IVCommon.h>
#include <media/hardware/HardwareAPI.h>
#include <media/hardware/MetadataBufferType.h>

#include <ui/GraphicBuffer.h>
#include <gui/BufferItem.h>
#include <gui/ISurfaceComposer.h>
#include <OMX_Component.h>

#include <utils/Log.h>
#include <utils/String8.h>

#include <private/gui/ComposerService.h>

namespace android {

SurfaceMediaSource::SurfaceMediaSource(uint32_t bufferWidth, uint32_t bufferHeight) :
    mWidth(bufferWidth),
    mHeight(bufferHeight),
    mCurrentSlot(BufferQueue::INVALID_BUFFER_SLOT),
    mNumPendingBuffers(0),
    mCurrentTimestamp(0),
    mFrameRate(30),
    mStarted(false),
    mNumFramesReceived(0),
    mNumFramesEncoded(0),
    mFirstFrameTimestamp(0),
    mMaxAcquiredBufferCount(4),  // XXX double-check the default
    mUseAbsoluteTimestamps(false) {
    ALOGV("SurfaceMediaSource");

    if (bufferWidth == 0 || bufferHeight == 0) {
        ALOGE("Invalid dimensions %dx%d", bufferWidth, bufferHeight);
    }

    BufferQueue::createBufferQueue(&mProducer, &mConsumer);
    mConsumer->setDefaultBufferSize(bufferWidth, bufferHeight);
mConsumer->setConsumerUsageBits(GRALLOC_USAGE_HW_VIDEO_ENCODER);

    sp<ISurfaceComposer> composer(ComposerService::getComposerService());

    // Note that we can't create an sp<...>(this) in a ctor that will not keep a
    // reference once the ctor ends, as that would cause the refcount of 'this'
    // dropping to 0 at the end of the ctor.  Since all we need is a wp<...>
    // that's what we create.
    wp<ConsumerListener> listener = static_cast<ConsumerListener*>(this);
    sp<BufferQueue::ProxyConsumerListener> proxy = new BufferQueue::ProxyConsumerListener(listener);

    status_t err = mConsumer->consumerConnect(proxy, false);
    if (err != NO_ERROR) {
        ALOGE("SurfaceMediaSource: error connecting to BufferQueue: %s (%d)",
                strerror(-err), err);
    }
}

SurfaceMediaSource::~SurfaceMediaSource() {
    ALOGV("~SurfaceMediaSource");
    CHECK(!mStarted);
}

nsecs_t SurfaceMediaSource::getTimestamp() {
    ALOGV("getTimestamp");
    Mutex::Autolock lock(mMutex);
    return mCurrentTimestamp;
}

void SurfaceMediaSource::setFrameAvailableListener(
        const sp<FrameAvailableListener>& listener) {
    ALOGV("setFrameAvailableListener");
    Mutex::Autolock lock(mMutex);
    mFrameAvailableListener = listener;
}

void SurfaceMediaSource::dumpState(String8& result) const
{
    char buffer[1024];
    dumpState(result, "", buffer, 1024);
}

void SurfaceMediaSource::dumpState(
        String8& result,
        const char* /* prefix */,
        char* buffer,
        size_t /* SIZE */) const
{
    Mutex::Autolock lock(mMutex);

    result.append(buffer);
    mConsumer->dumpState(result, "");
}

status_t SurfaceMediaSource::setFrameRate(int32_t fps)
{
    ALOGV("setFrameRate");
    Mutex::Autolock lock(mMutex);
    const int MAX_FRAME_RATE = 60;
    if (fps < 0 || fps > MAX_FRAME_RATE) {
        return BAD_VALUE;
    }
    mFrameRate = fps;
    return OK;
}

MetadataBufferType SurfaceMediaSource::metaDataStoredInVideoBuffers() const {
    ALOGV("isMetaDataStoredInVideoBuffers");
    return kMetadataBufferTypeANWBuffer;
}

int32_t SurfaceMediaSource::getFrameRate( ) const {
    ALOGV("getFrameRate");
    Mutex::Autolock lock(mMutex);
    return mFrameRate;
}

status_t SurfaceMediaSource::start(MetaData *params)
{
    ALOGV("start");

    Mutex::Autolock lock(mMutex);

    CHECK(!mStarted);

    mStartTimeNs = 0;
    int64_t startTimeUs;
    int32_t bufferCount = 0;
    if (params) {
        if (params->findInt64(kKeyTime, &startTimeUs)) {
            mStartTimeNs = startTimeUs * 1000;
        }

        if (!params->findInt32(kKeyNumBuffers, &bufferCount)) {
            ALOGE("Failed to find the advertised buffer count");
            return UNKNOWN_ERROR;
        }

        if (bufferCount <= 1) {
            ALOGE("bufferCount %d is too small", bufferCount);
            return BAD_VALUE;
        }

        mMaxAcquiredBufferCount = bufferCount;
    }

    CHECK_GT(mMaxAcquiredBufferCount, 1u);

    status_t err =
        mConsumer->setMaxAcquiredBufferCount(mMaxAcquiredBufferCount);

    if (err != OK) {
        return err;
    }

    mNumPendingBuffers = 0;
    mStarted = true;

    return OK;
}

status_t SurfaceMediaSource::setMaxAcquiredBufferCount(size_t count) {
    ALOGV("setMaxAcquiredBufferCount(%zu)", count);
    Mutex::Autolock lock(mMutex);

    CHECK_GT(count, 1u);
    mMaxAcquiredBufferCount = count;

    return OK;
}

status_t SurfaceMediaSource::setUseAbsoluteTimestamps() {
    ALOGV("setUseAbsoluteTimestamps");
    Mutex::Autolock lock(mMutex);
    mUseAbsoluteTimestamps = true;

    return OK;
}

status_t SurfaceMediaSource::stop()
{
    ALOGV("stop");
    Mutex::Autolock lock(mMutex);

    if (!mStarted) {
        return OK;
    }

    mStarted = false;
    mFrameAvailableCondition.signal();

    while (mNumPendingBuffers > 0) {
        ALOGI("Still waiting for %zu buffers to be returned.",
                mNumPendingBuffers);

#if DEBUG_PENDING_BUFFERS
        for (size_t i = 0; i < mPendingBuffers.size(); ++i) {
            ALOGI("%zu: %p", i, mPendingBuffers.itemAt(i));
        }
#endif

        mMediaBuffersAvailableCondition.wait(mMutex);
    }

    mMediaBuffersAvailableCondition.signal();

    return mConsumer->consumerDisconnect();
}

sp<MetaData> SurfaceMediaSource::getFormat()
{
    ALOGV("getFormat");

    Mutex::Autolock lock(mMutex);
    sp<MetaData> meta = new MetaData;

    meta->setInt32(kKeyWidth, mWidth);
    meta->setInt32(kKeyHeight, mHeight);
    // The encoder format is set as an opaque colorformat
    // The encoder will later find out the actual colorformat
    // from the GL Frames itself.
    meta->setInt32(kKeyColorFormat, OMX_COLOR_FormatAndroidOpaque);
    meta->setInt32(kKeyStride, mWidth);
    meta->setInt32(kKeySliceHeight, mHeight);
    meta->setInt32(kKeyFrameRate, mFrameRate);
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);
    return meta;
}

// Pass the data to the MediaBuffer. Pass in only the metadata
// Note: Call only when you have the lock
void SurfaceMediaSource::passMetadataBuffer_l(MediaBufferBase **buffer,
        ANativeWindowBuffer *bufferHandle) const {
    *buffer = new MediaBuffer(sizeof(VideoNativeMetadata));
    VideoNativeMetadata *data = (VideoNativeMetadata *)(*buffer)->data();
    if (data == NULL) {
        ALOGE("Cannot allocate memory for metadata buffer!");
        return;
    }
    data->eType = metaDataStoredInVideoBuffers();
    data->pBuffer = bufferHandle;
    data->nFenceFd = -1;
    ALOGV("handle = %p, offset = %zu, length = %zu",
            bufferHandle, (*buffer)->range_length(), (*buffer)->range_offset());
}

status_t SurfaceMediaSource::read(
        MediaBufferBase **buffer, const ReadOptions * /* options */) {
    ALOGV("read");
    Mutex::Autolock lock(mMutex);

    *buffer = NULL;

    while (mStarted && mNumPendingBuffers == mMaxAcquiredBufferCount) {
        mMediaBuffersAvailableCondition.wait(mMutex);
    }

    // Update the current buffer info
    // TODO: mCurrentSlot can be made a bufferstate since there
    // can be more than one "current" slots.

    BufferItem item;
    // If the recording has started and the queue is empty, then just
    // wait here till the frames come in from the client side
    while (mStarted) {

        status_t err = mConsumer->acquireBuffer(&item, 0);
        if (err == BufferQueue::NO_BUFFER_AVAILABLE) {
            // wait for a buffer to be queued
            mFrameAvailableCondition.wait(mMutex);
        } else if (err == OK) {
            err = item.mFence->waitForever("SurfaceMediaSource::read");
            if (err) {
                ALOGW("read: failed to wait for buffer fence: %d", err);
            }

            // First time seeing the buffer?  Added it to the SMS slot
            if (item.mGraphicBuffer != NULL) {
                mSlots[item.mSlot].mGraphicBuffer = item.mGraphicBuffer;
            }
            mSlots[item.mSlot].mFrameNumber = item.mFrameNumber;

            // check for the timing of this buffer
            if (mNumFramesReceived == 0 && !mUseAbsoluteTimestamps) {
                mFirstFrameTimestamp = item.mTimestamp;
                // Initial delay
                if (mStartTimeNs > 0) {
                    if (item.mTimestamp < mStartTimeNs) {
                        // This frame predates start of record, discard
                        mConsumer->releaseBuffer(
                                item.mSlot, item.mFrameNumber, EGL_NO_DISPLAY,
                                EGL_NO_SYNC_KHR, Fence::NO_FENCE);
                        continue;
                    }
                    mStartTimeNs = item.mTimestamp - mStartTimeNs;
                }
            }
            item.mTimestamp = mStartTimeNs + (item.mTimestamp - mFirstFrameTimestamp);

            mNumFramesReceived++;

            break;
        } else {
            ALOGE("read: acquire failed with error code %d", err);
            return ERROR_END_OF_STREAM;
        }

    }

    // If the loop was exited as a result of stopping the recording,
    // it is OK
    if (!mStarted) {
        ALOGV("Read: SurfaceMediaSource is stopped. Returning ERROR_END_OF_STREAM.");
        return ERROR_END_OF_STREAM;
    }

    mCurrentSlot = item.mSlot;

    // First time seeing the buffer?  Added it to the SMS slot
    if (item.mGraphicBuffer != NULL) {
        mSlots[item.mSlot].mGraphicBuffer = item.mGraphicBuffer;
    }
    mSlots[item.mSlot].mFrameNumber = item.mFrameNumber;

    mCurrentBuffers.push_back(mSlots[mCurrentSlot].mGraphicBuffer);
    int64_t prevTimeStamp = mCurrentTimestamp;
    mCurrentTimestamp = item.mTimestamp;

    mNumFramesEncoded++;
    // Pass the data to the MediaBuffer. Pass in only the metadata

    passMetadataBuffer_l(buffer, mSlots[mCurrentSlot].mGraphicBuffer->getNativeBuffer());

    (*buffer)->setObserver(this);
    (*buffer)->add_ref();
    (*buffer)->meta_data().setInt64(kKeyTime, mCurrentTimestamp / 1000);
    ALOGV("Frames encoded = %d, timestamp = %" PRId64 ", time diff = %" PRId64,
            mNumFramesEncoded, mCurrentTimestamp / 1000,
            mCurrentTimestamp / 1000 - prevTimeStamp / 1000);

    ++mNumPendingBuffers;

#if DEBUG_PENDING_BUFFERS
    mPendingBuffers.push_back(*buffer);
#endif

    ALOGV("returning mbuf %p", *buffer);

    return OK;
}

static buffer_handle_t getMediaBufferHandle(MediaBufferBase *buffer) {
    // need to convert to char* for pointer arithmetic and then
    // copy the byte stream into our handle
    buffer_handle_t bufferHandle;
    VideoNativeMetadata *data = (VideoNativeMetadata *)buffer->data();
    ANativeWindowBuffer *anwbuffer = (ANativeWindowBuffer *)data->pBuffer;
    bufferHandle = anwbuffer->handle;
    return bufferHandle;
}

void SurfaceMediaSource::signalBufferReturned(MediaBufferBase *buffer) {
    ALOGV("signalBufferReturned");

    bool foundBuffer = false;

    Mutex::Autolock lock(mMutex);

    buffer_handle_t bufferHandle = getMediaBufferHandle(buffer);
    ANativeWindowBuffer* curNativeHandle = NULL;

    for (size_t i = 0; i < mCurrentBuffers.size(); i++) {
        curNativeHandle = mCurrentBuffers[i]->getNativeBuffer();
        if ((mCurrentBuffers[i]->handle == bufferHandle) ||
            ((buffer_handle_t)curNativeHandle == bufferHandle)) {
            mCurrentBuffers.removeAt(i);
            foundBuffer = true;
            break;
        }
    }

    if (!foundBuffer) {
        ALOGW("returned buffer was not found in the current buffer list");
    }

    for (int id = 0; id < BufferQueue::NUM_BUFFER_SLOTS; id++) {
        if (mSlots[id].mGraphicBuffer == NULL) {
            continue;
        }

        curNativeHandle = mSlots[id].mGraphicBuffer->getNativeBuffer();

        if ((bufferHandle == mSlots[id].mGraphicBuffer->handle) ||
            (bufferHandle == (buffer_handle_t)curNativeHandle)) {
            ALOGV("Slot %d returned, matches handle = %p", id,
                    mSlots[id].mGraphicBuffer->handle);

            mConsumer->releaseBuffer(id, mSlots[id].mFrameNumber,
                                        EGL_NO_DISPLAY, EGL_NO_SYNC_KHR,
                    Fence::NO_FENCE);

            buffer->setObserver(0);
            buffer->release();

            foundBuffer = true;
            break;
        }
    }

    if (!foundBuffer) {
        CHECK(!"signalBufferReturned: bogus buffer");
    }

#if DEBUG_PENDING_BUFFERS
    for (size_t i = 0; i < mPendingBuffers.size(); ++i) {
        if (mPendingBuffers.itemAt(i) == buffer) {
            mPendingBuffers.removeAt(i);
            break;
        }
    }
#endif

    --mNumPendingBuffers;
    mMediaBuffersAvailableCondition.broadcast();
}

// Part of the BufferQueue::ConsumerListener
void SurfaceMediaSource::onFrameAvailable(const BufferItem& /* item */) {
    ALOGV("onFrameAvailable");

    sp<FrameAvailableListener> listener;
    { // scope for the lock
        Mutex::Autolock lock(mMutex);
        mFrameAvailableCondition.broadcast();
        listener = mFrameAvailableListener;
    }

    if (listener != NULL) {
        ALOGV("actually calling onFrameAvailable");
        listener->onFrameAvailable();
    }
}

// SurfaceMediaSource hijacks this event to assume
// the prodcuer is disconnecting from the BufferQueue
// and that it should stop the recording
void SurfaceMediaSource::onBuffersReleased() {
    ALOGV("onBuffersReleased");

    Mutex::Autolock lock(mMutex);

    mFrameAvailableCondition.signal();

    for (int i = 0; i < BufferQueue::NUM_BUFFER_SLOTS; i++) {
       mSlots[i].mGraphicBuffer = 0;
    }
}

void SurfaceMediaSource::onSidebandStreamChanged() {
    ALOG_ASSERT(false, "SurfaceMediaSource can't consume sideband streams");
}

} // end of namespace android
