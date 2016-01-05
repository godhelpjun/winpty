// Copyright (c) 2011-2012 Ryan Prichard
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include <string.h>

#include <algorithm>

#include "EventLoop.h"
#include "NamedPipe.h"
#include "../shared/DebugClient.h"
#include "../shared/WinptyAssert.h"

NamedPipe::NamedPipe() :
    m_readBufferSize(64 * 1024),
    m_handle(NULL),
    m_inputWorker(NULL),
    m_outputWorker(NULL)
{
}

NamedPipe::~NamedPipe()
{
    closePipe();
}

// Returns true if anything happens (data received, data sent, pipe error).
bool NamedPipe::serviceIo(std::vector<HANDLE> *waitHandles)
{
    if (m_handle == NULL) {
        return false;
    }
    int readBytes = 0;
    int writeBytes = 0;
    HANDLE readHandle = NULL;
    HANDLE writeHandle = NULL;
    if (m_inputWorker != NULL) {
        readBytes = m_inputWorker->service();
        readHandle = m_inputWorker->getWaitEvent();
    }
    if (m_outputWorker != NULL) {
        writeBytes = m_outputWorker->service();
        writeHandle = m_outputWorker->getWaitEvent();
    }
    if (readBytes == -1 || writeBytes == -1) {
        closePipe();
        return true;
    }
    if (readHandle != NULL) { waitHandles->push_back(readHandle); }
    if (writeHandle != NULL) { waitHandles->push_back(writeHandle); }
    return readBytes > 0 || writeBytes > 0;
}

NamedPipe::IoWorker::IoWorker(NamedPipe *namedPipe) :
    m_namedPipe(namedPipe),
    m_pending(false),
    m_currentIoSize(-1)
{
    m_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT(m_event != NULL);
}

NamedPipe::IoWorker::~IoWorker()
{
    CloseHandle(m_event);
}

int NamedPipe::IoWorker::service()
{
    int progress = 0;
    if (m_pending) {
        DWORD actual;
        BOOL ret = GetOverlappedResult(m_namedPipe->m_handle, &m_over, &actual, FALSE);
        if (!ret) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) {
                // There is a pending I/O.
                return progress;
            } else {
                // Pipe error.
                return -1;
            }
        }
        ResetEvent(m_event);
        m_pending = false;
        completeIo(actual);
        m_currentIoSize = -1;
        progress += actual;
    }
    int nextSize;
    bool isRead;
    while (shouldIssueIo(&nextSize, &isRead)) {
        m_currentIoSize = nextSize;
        DWORD actual = 0;
        memset(&m_over, 0, sizeof(m_over));
        m_over.hEvent = m_event;
        BOOL ret = isRead
                ? ReadFile(m_namedPipe->m_handle, m_buffer, nextSize, &actual, &m_over)
                : WriteFile(m_namedPipe->m_handle, m_buffer, nextSize, &actual, &m_over);
        if (!ret) {
            if (GetLastError() == ERROR_IO_PENDING) {
                // There is a pending I/O.
                m_pending = true;
                return progress;
            } else {
                // Pipe error.
                return -1;
            }
        }
        ResetEvent(m_event);
        completeIo(actual);
        m_currentIoSize = -1;
        progress += actual;
    }
    return progress;
}

// This function is called after CancelIo has returned.  We need to block until
// the I/O operations have completed, which should happen very quickly.
// https://blogs.msdn.microsoft.com/oldnewthing/20110202-00/?p=11613
void NamedPipe::IoWorker::waitForCanceledIo()
{
    if (m_pending) {
        DWORD actual = 0;
        GetOverlappedResult(m_namedPipe->m_handle, &m_over, &actual, TRUE);
        m_pending = false;
    }
}

HANDLE NamedPipe::IoWorker::getWaitEvent()
{
    return m_pending ? m_event : NULL;
}

void NamedPipe::InputWorker::completeIo(int size)
{
    m_namedPipe->m_inQueue.append(m_buffer, size);
}

bool NamedPipe::InputWorker::shouldIssueIo(int *size, bool *isRead)
{
    *isRead = true;
    if (m_namedPipe->isClosed()) {
        return false;
    } else if ((int)m_namedPipe->m_inQueue.size() < m_namedPipe->readBufferSize()) {
        *size = kIoSize;
        return true;
    } else {
        return false;
    }
}

void NamedPipe::OutputWorker::completeIo(int size)
{
    ASSERT(size == m_currentIoSize);
}

bool NamedPipe::OutputWorker::shouldIssueIo(int *size, bool *isRead)
{
    *isRead = false;
    if (!m_namedPipe->m_outQueue.empty()) {
        int writeSize = std::min((int)m_namedPipe->m_outQueue.size(), (int)kIoSize);
        memcpy(m_buffer, m_namedPipe->m_outQueue.data(), writeSize);
        m_namedPipe->m_outQueue.erase(0, writeSize);
        *size = writeSize;
        return true;
    } else {
        return false;
    }
}

int NamedPipe::OutputWorker::getPendingIoSize()
{
    return m_pending ? m_currentIoSize : 0;
}

// Connect to an existing named pipe.
bool NamedPipe::connectToServer(const std::wstring &name)
{
    ASSERT(isClosed());
    m_name = name;
    HANDLE handle = CreateFileW(name.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED,
                                NULL);
    trace("connection to [%ls], handle == 0x%x", name.c_str(), handle);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    m_handle = handle;
    m_inputWorker = new InputWorker(this);
    m_outputWorker = new OutputWorker(this);
    return true;
}

// Block until the server pipe is connected to a client, or kill the agent
// process if the connect times out.
void NamedPipe::connectToClient()
{
    ASSERT(!isClosed());
    OVERLAPPED over = {};
    over.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT(over.hEvent != NULL);
    BOOL success = ConnectNamedPipe(m_handle, &over);
    if (!success && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(over.hEvent, 30000);
        DWORD actual = 0;
        success = GetOverlappedResult(m_handle, &over, &actual, FALSE);
    }
    if (!success && GetLastError() == ERROR_PIPE_CONNECTED) {
        success = true;
    }
    ASSERT(success && "error connecting data I/O pipe");
    CloseHandle(over.hEvent);
}

// Bypass the output queue and event loop.  Block until the data is written,
// or kill the agent process if the write times out.
void NamedPipe::writeImmediately(const void *data, int size)
{
    ASSERT(m_outputWorker != NULL);
    ASSERT(!m_outputWorker->ioPending());
    OVERLAPPED over = {};
    over.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT(over.hEvent != NULL);
    DWORD actual = 0;
    BOOL success = WriteFile(m_handle, data, size, &actual, &over);
    if (!success && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(over.hEvent, 30000);
        success = GetOverlappedResult(m_handle, &over, &actual, FALSE);
    }
    ASSERT(success && actual == static_cast<DWORD>(size) &&
        "error writing data to pipe");
    CloseHandle(over.hEvent);
}

// Adopt a handle for an already-open named pipe instance.
void NamedPipe::adoptHandle(HANDLE handle, bool write, const std::wstring &name)
{
    ASSERT(isClosed());
    m_name = name;
    m_handle = handle;
    if (write) {
        m_outputWorker = new OutputWorker(this);
    } else {
        m_inputWorker = new InputWorker(this);
    }
}

int NamedPipe::bytesToSend()
{
    int ret = m_outQueue.size();
    if (m_outputWorker != NULL)
        ret += m_outputWorker->getPendingIoSize();
    return ret;
}

void NamedPipe::write(const void *data, int size)
{
    m_outQueue.append((const char*)data, size);
}

void NamedPipe::write(const char *text)
{
    write(text, strlen(text));
}

int NamedPipe::readBufferSize()
{
    return m_readBufferSize;
}

void NamedPipe::setReadBufferSize(int size)
{
    m_readBufferSize = size;
}

int NamedPipe::bytesAvailable()
{
    return m_inQueue.size();
}

int NamedPipe::peek(void *data, int size)
{
    int ret = std::min(size, (int)m_inQueue.size());
    memcpy(data, m_inQueue.data(), ret);
    return ret;
}

std::string NamedPipe::read(int size)
{
    int retSize = std::min(size, (int)m_inQueue.size());
    std::string ret = m_inQueue.substr(0, retSize);
    m_inQueue.erase(0, retSize);
    return ret;
}

std::vector<char> NamedPipe::readAsVector(int size)
{
    const auto retSize = std::min<size_t>(size, m_inQueue.size());
    std::vector<char> ret(retSize);
    if (retSize > 0) {
        const char *const p = &m_inQueue[0];
        std::copy(p, p + retSize, ret.begin());
        m_inQueue.erase(0, retSize);
    }
    return ret;
}

std::string NamedPipe::readAll()
{
    std::string ret = m_inQueue;
    m_inQueue.clear();
    return ret;
}

void NamedPipe::closePipe()
{
    if (m_handle == NULL) {
        return;
    }
    CancelIo(m_handle);
    if (m_inputWorker != NULL) {
        m_inputWorker->waitForCanceledIo();
    }
    if (m_outputWorker != NULL) {
        m_outputWorker->waitForCanceledIo();
    }
    delete m_inputWorker;
    delete m_outputWorker;
    CloseHandle(m_handle);
    m_handle = NULL;
    m_inputWorker = NULL;
    m_outputWorker = NULL;
}

bool NamedPipe::isClosed()
{
    return m_handle == NULL;
}
