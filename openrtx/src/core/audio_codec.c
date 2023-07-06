/***************************************************************************
 *   Copyright (C) 2022 - 2023 by Federico Amedeo Izzo IU2NUO,             *
 *                                Niccolò Izzo IU2KIN                      *
 *                                Frederik Saraci IU2NRO                   *
 *                                Silvano Seva IU2KWO                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/

#include <audio_stream.h>
#include <audio_codec.h>
#include <pthread.h>
#include <codec2.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dsp.h>

#define BUF_SIZE 4

static pathId           audioPath;

static uint8_t          initCnt = 0;
static bool             running;

static bool             reqStop;
static pthread_t        codecThread;
static pthread_mutex_t  mutex;
static pthread_cond_t   wakeup_cond;

static uint8_t          readPos;
static uint8_t          writePos;
static uint8_t          numElements;
static uint64_t         dataBuffer[BUF_SIZE];

#ifdef PLATFORM_MOD17
static const uint8_t micGainPre  = 4;
static const uint8_t micGainPost = 3;
#else
static const uint8_t micGainPre  = 8;
static const uint8_t micGainPost = 4;
#endif

static void *encodeFunc(void *arg);
static void *decodeFunc(void *arg);
static void startThread(void *(*func) (void *));
static void stopThread();


void codec_init()
{
    if(initCnt > 0)
    {
        pthread_mutex_lock(&mutex);
        initCnt += 1;
        pthread_mutex_unlock(&mutex);
        return;
    }
    else
    {
        initCnt = 1;
    }

    running     = false;
    readPos     = 0;
    writePos    = 0;
    numElements = 0;
    memset(dataBuffer, 0x00, BUF_SIZE * sizeof(uint64_t));

    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&wakeup_cond, NULL);
}

void codec_terminate()
{
    pthread_mutex_lock(&mutex);
    initCnt -= 1;
    pthread_mutex_unlock(&mutex);

    if(initCnt > 0) return;
    if(running) stopThread();

    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&wakeup_cond);
}

bool codec_startEncode(const pathId path)
{
    // Bad incoming path
    if(audioPath_getStatus(path) != PATH_OPEN)
        return false;

    if(running)
    {
        if(audioPath_getStatus(audioPath) == PATH_OPEN)
            return false;
        else
            stopThread();
    }

    running   = true;
    audioPath = path;
    startThread(encodeFunc);

    return true;
}

bool codec_startDecode(const pathId path)
{
    // Bad incoming path
    if(audioPath_getStatus(path) != PATH_OPEN)
        return false;

    if(running)
    {
        if(audioPath_getStatus(audioPath) == PATH_OPEN)
            return false;
        else
            stopThread();
    }

    running   = true;
    audioPath = path;
    startThread(decodeFunc);

    return true;
}

void codec_stop(const pathId path)
{
    if(running == false)
        return;

    if(audioPath != path)
        return;

    stopThread();
}

int codec_popFrame(uint8_t *frame, const bool blocking)
{
    if(running == false)
        return -EPERM;

    uint64_t element;

    // No data available and non-blocking call: just return false.
    if((numElements == 0) && (blocking == false))
        return -EAGAIN;

    // Blocking call: wait until some data is pushed
    pthread_mutex_lock(&mutex);
    while(numElements == 0)
    {
        pthread_cond_wait(&wakeup_cond, &mutex);
    }

    element      = dataBuffer[readPos];
    readPos      = (readPos + 1) % BUF_SIZE;
    numElements -= 1;
    pthread_mutex_unlock(&mutex);

    // Do memcpy after mutex unlock to reduce execution time spent inside the
    // critical section
    memcpy(frame, &element, 8);

    return 0;
}

int codec_pushFrame(const uint8_t *frame, const bool blocking)
{
    if(running == false)
        return -EPERM;

    // Copy data to a temporary variable before mutex lock to reduce execution
    // time spent inside the critical section
    uint64_t element;
    memcpy(&element, frame, 8);


    // No space available and non-blocking call: return
    if((numElements >= BUF_SIZE) && (blocking == false))
        return -EAGAIN;

    // Blocking call: wait until there is some free space
    pthread_mutex_lock(&mutex);
    while(numElements >= BUF_SIZE)
    {
        pthread_cond_wait(&wakeup_cond, &mutex);
    }

    // There is free space, push data into the queue
    dataBuffer[writePos] = element;
    writePos = (writePos + 1) % BUF_SIZE;

    // Signal that the queue is not empty
    if(numElements == 0) pthread_cond_signal(&not_empty);
    numElements += 1;

    pthread_mutex_unlock(&mutex);
    return 0;
}




static void *encodeFunc(void *arg)
{

    streamId        iStream;
    pathId          iPath = (pathId) arg;
    stream_sample_t audioBuf[320];
    struct CODEC2   *codec2;
    filter_state_t  dcrState;

    iStream = audioStream_start(iPath, audioBuf, 320, 8000,
                                STREAM_INPUT | BUF_CIRC_DOUBLE);
    if(iStream < 0)
    {
        running = false;
        return NULL;
    }

    dsp_resetFilterState(&dcrState);
    codec2 = codec2_create(CODEC2_MODE_3200);

    while(reqStop == false)
    {
        // Invalid path, quit
        if(audioPath_getStatus(iPath) != PATH_OPEN)
            break;

        dataBlock_t audio = inputStream_getData(iStream);

        if(audio.data != NULL)
        {
            #ifndef PLATFORM_LINUX
            // Pre-amplification stage
            for(size_t i = 0; i < audio.len; i++) audio.data[i] *= micGainPre;

            // DC removal
            dsp_dcRemoval(&dcrState, audio.data, audio.len);

            // Post-amplification stage
            for(size_t i = 0; i < audio.len; i++) audio.data[i] *= micGainPost;
            #endif

            // CODEC2 encodes 160ms of speech into 8 bytes: here we write the
            // new encoded data into a buffer of 16 bytes writing the first
            // half and then the second one, sequentially.
            // Data ready flag is rised once all the 16 bytes contain new data.
            uint64_t frame = 0;
            codec2_encode(codec2, ((uint8_t*) &frame), audio.data);

            pthread_mutex_lock(&mutex);

            // If buffer is full erase the oldest frame
            if(numElements >= BUF_SIZE)
            {
                readPos = (readPos + 1) % BUF_SIZE;
            }

            dataBuffer[writePos] = frame;
            writePos = (writePos + 1) % BUF_SIZE;

            if(numElements == 0)
                pthread_cond_signal(&wakeup_cond);

            if(numElements < BUF_SIZE)
                numElements += 1;

            pthread_mutex_unlock(&mutex);
        }
    }

    audioStream_terminate(iStream);
    codec2_destroy(codec2);

    return NULL;
}

static void *decodeFunc(void *arg)
{
    streamId        oStream;
    pathId          oPath = (pathId) arg;
    stream_sample_t audioBuf[320];
    struct CODEC2   *codec2;

    // Open output stream
    memset(audioBuf, 0x00, 320 * sizeof(stream_sample_t));
    oStream = audioStream_start(oPath, audioBuf, 320, 8000,
                                STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if(oStream < 0)
    {
        running = false;
        return NULL;
    }

    codec2 = codec2_create(CODEC2_MODE_3200);

    // Ensure that thread start is correctly synchronized with the output
    // stream to avoid having the decode function writing in a memory area
    // being read at the same time by the output stream system causing cracking
    // noises at speaker output. Behaviour observed on both Module17 and MD-UV380
    outputStream_sync(oStream, false);

    while(reqStop == false)
    {
        // Invalid path, quit
        if(audioPath_getStatus(oPath) != PATH_OPEN)
            break;

        // Try popping data from the queue
        uint64_t frame   = 0;
        bool     newData = false;

        pthread_mutex_lock(&mutex);

        if(numElements != 0)
        {
            frame        = dataBuffer[readPos];
            readPos      = (readPos + 1) % BUF_SIZE;
            if(numElements >= BUF_SIZE)
                pthread_cond_signal(&wakeup_cond);

            numElements -= 1;
            newData      = true;
        }

        pthread_mutex_unlock(&mutex);

        stream_sample_t *audioBuf = outputStream_getIdleBuffer(oStream);

        if(newData)
        {
            codec2_decode(codec2, audioBuf, ((uint8_t *) &frame));

            #ifdef PLATFORM_MD3x0
            // Bump up volume a little bit, as on MD3x0 is quite low
            for(size_t i = 0; i < 160; i++) audioBuf[i] *= 2;
            #endif

        }
        else
        {
            memset(audioBuf, 0x00, 160 * sizeof(stream_sample_t));
        }

        outputStream_sync(oStream, true);
    }

    // Stop stream and wait until its effective termination
    audioStream_stop(oStream);
    codec2_destroy(codec2);

    return NULL;
}

static void startThread(void *(*func) (void *))
{
    readPos     = 0;
    writePos    = 0;
    numElements = 0;
    reqStop     = false;

    #ifdef _MIOSIX
    // Set stack size of CODEC2 thread to 16kB.
    pthread_attr_t codecAttr;
    pthread_attr_init(&codecAttr);
    pthread_attr_setstacksize(&codecAttr, 16384);

    // Set priority of CODEC2 thread to the maximum one, the same of RTX thread.
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(0);
    pthread_attr_setschedparam(&codecAttr, &param);

    // Start thread
    pthread_create(&codecThread, &codecAttr, func, ((void *) audioPath));
    #else
    pthread_create(&codecThread, NULL, func, ((void *) audioPath));
    #endif

}

static void stopThread()
{
    reqStop = true;
    pthread_join(codecThread, NULL);
    running = false;
}
