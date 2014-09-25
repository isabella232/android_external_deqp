/*-------------------------------------------------------------------------
 * drawElements Quality Program Tester Core
 * ----------------------------------------
 *
 * Copyright 2014 The Android Open Source Project
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
 *
 *//*!
 * \file
 * \brief RenderActivity base class.
 *//*--------------------------------------------------------------------*/

#include "tcuAndroidRenderActivity.hpp"
#include "deSemaphore.hpp"

#include <android/window.h>

#include <string>
#include <stdlib.h>

using std::string;

#if defined(DE_DEBUG)
#	define DBG_PRINT(X) print X
#else
#	define DBG_PRINT(X)
#endif

namespace tcu
{
namespace Android
{

enum
{
	MESSAGE_QUEUE_SIZE = 8 //!< Length of RenderThread message queue.
};

// RenderThread

RenderThread::RenderThread (NativeActivity& activity)
	: m_activity		(activity)
	, m_msgQueue		(MESSAGE_QUEUE_SIZE)
	, m_threadRunning	(false)
	, m_inputQueue		(DE_NULL)
	, m_windowState		(WINDOWSTATE_NOT_CREATED)
	, m_window			(DE_NULL)
	, m_paused			(false)
	, m_finish			(false)
{
}

RenderThread::~RenderThread (void)
{
}

void RenderThread::start (void)
{
	m_threadRunning = true;
	Thread::start();
}

void RenderThread::destroy (void)
{
	// Queue finish command
	enqueue(Message(MESSAGE_FINISH));

	// Wait for thread to terminate
	join();

	m_threadRunning = false;
}

void RenderThread::enqueue (const Message& message)
{
	// \note Thread must be running or otherwise nobody is going to drain the queue.
	DE_ASSERT(m_threadRunning);
	m_msgQueue.pushFront(message);
}

void RenderThread::pause (void)
{
	enqueue(Message(MESSAGE_PAUSE));
}

void RenderThread::resume (void)
{
	enqueue(Message(MESSAGE_RESUME));
}

void RenderThread::sync (void)
{
	de::Semaphore waitSem(0);
	enqueue(Message(MESSAGE_SYNC, &waitSem));
	waitSem.decrement();
}

void RenderThread::processMessage (const Message& message)
{
	switch (message.type)
	{
		case MESSAGE_RESUME:	m_paused = false;	break;
		case MESSAGE_PAUSE:		m_paused = true;	break;
		case MESSAGE_FINISH:	m_finish = true;	break;

		case MESSAGE_WINDOW_CREATED:
			if (m_windowState != WINDOWSTATE_NOT_CREATED && m_windowState != WINDOWSTATE_DESTROYED)
				throw InternalError("Got unexpected onNativeWindowCreated() event from system");
			m_windowState	= WINDOWSTATE_NOT_INITIALIZED;
			m_window		= (ANativeWindow*)message.data;
			break;

		case MESSAGE_WINDOW_RESIZED:
			DE_ASSERT(m_window == message.data);
			if (m_windowState == WINDOWSTATE_NOT_INITIALIZED)
			{
				// Got first resize event, window is ready for use.
				m_windowState = WINDOWSTATE_READY;
				onWindowCreated((ANativeWindow*)message.data);
			}
			else if (m_windowState == WINDOWSTATE_READY)
				onWindowResized((ANativeWindow*)message.data);
			else
				throw InternalError("Got unexpected onNativeWindowResized() event from system");
			break;

		case MESSAGE_WINDOW_DESTROYED:
			DE_ASSERT(m_window == message.data);
			if (m_windowState != WINDOWSTATE_NOT_INITIALIZED && m_windowState != WINDOWSTATE_READY)
				throw InternalError("Got unexpected onNativeWindowDestroyed() event from system");

			onWindowDestroyed((ANativeWindow*)message.data);
			m_windowState	= WINDOWSTATE_DESTROYED;
			m_window		= DE_NULL;
			break;

		case MESSAGE_INPUT_QUEUE_CREATED:
			m_inputQueue = (AInputQueue*)message.data;
			break;

		case MESSAGE_INPUT_QUEUE_DESTROYED:
			m_inputQueue = (AInputQueue*)message.data;
			break;

		case MESSAGE_SYNC:
			static_cast<de::Semaphore*>(message.data)->increment();
			break;

		default:
			throw std::runtime_error("Unknown message type");
			break;
	}
}

void RenderThread::run (void)
{
	// Init state
	m_windowState	= WINDOWSTATE_NOT_CREATED;
	m_paused		= true;
	m_finish		= false;

	try
	{
		while (!m_finish)
		{
			if (m_paused || m_windowState != WINDOWSTATE_READY)
			{
				// Block until we are not paused and window is ready.
				Message msg = m_msgQueue.popBack();
				processMessage(msg);
				continue;
			}

			// Process available commands
			{
				Message msg;
				if (m_msgQueue.tryPopBack(msg))
				{
					processMessage(msg);
					continue;
				}
			}

			DE_ASSERT(m_windowState == WINDOWSTATE_READY);

			// Process input events.
			// \todo [2013-05-08 pyry] What if system fills up the input queue before we have window ready?
			while (m_inputQueue &&
				   AInputQueue_hasEvents(m_inputQueue) > 0)
			{
				AInputEvent* event;
				TCU_CHECK(AInputQueue_getEvent(m_inputQueue, &event) >= 0);
				onInputEvent(event);
				AInputQueue_finishEvent(m_inputQueue, event, 1);
			}

			// Everything set up - safe to render.
			if (!render())
				break;
		}

		if (m_windowState == WINDOWSTATE_READY)
		{
			// Clean up window use
			onWindowDestroyed(m_window);
			m_windowState	= WINDOWSTATE_DESTROYED;
			m_window		= DE_NULL;
		}
	}
	catch (const std::exception& e)
	{
		print("RenderThread: %s\n", e.what());
	}

	// Tell activity to finish.
	DBG_PRINT(("RenderThread::run(): done, waiting for FINISH\n"));
	m_activity.finish();

	// Thread must keep draining message queue until FINISH message is encountered.
	try
	{
		while (!m_finish)
		{
			Message msg = m_msgQueue.popBack();

			// Ignore all but SYNC and FINISH messages.
			if (msg.type == MESSAGE_SYNC || msg.type == MESSAGE_FINISH)
				processMessage(msg);
		}
	}
	catch (const std::exception& e)
	{
		die("RenderThread: %s\n", e.what());
	}

	DBG_PRINT(("RenderThread::run(): exiting...\n"));
}

// RenderActivity

RenderActivity::RenderActivity (ANativeActivity* activity)
	: NativeActivity(activity)
	, m_thread		(DE_NULL)
{
	DBG_PRINT(("RenderActivity::RenderActivity()"));
}

RenderActivity::~RenderActivity (void)
{
	DBG_PRINT(("RenderActivity::~RenderActivity()"));
}

void RenderActivity::setThread (RenderThread* thread)
{
	DE_ASSERT(!m_thread && thread);
	m_thread = thread;
}

void RenderActivity::onStart (void)
{
	DBG_PRINT(("RenderActivity::onStart()"));

	// Launch tester thread when activity is created. It will remain in paused state until resume() is called.
	m_thread->start();
}

void RenderActivity::onResume (void)
{
	DBG_PRINT(("RenderActivity::onResume()"));

	// Resume (or start) test execution
	m_thread->resume();
}

void RenderActivity::onPause (void)
{
	DBG_PRINT(("RenderActivity::onPause()"));

	// Pause test execution
	m_thread->pause();
}

void RenderActivity::onStop (void)
{
	DBG_PRINT(("RenderActivity::onStop()"));
	m_thread->destroy();
}

void RenderActivity::onDestroy (void)
{
	DBG_PRINT(("RenderActivity::onDestroy()"));
}

void RenderActivity::onNativeWindowCreated (ANativeWindow* window)
{
	DBG_PRINT(("RenderActivity::onNativeWindowCreated()"));
	m_thread->enqueue(Message(MESSAGE_WINDOW_CREATED, window));
}

void RenderActivity::onNativeWindowResized (ANativeWindow* window)
{
	DBG_PRINT(("RenderActivity::onNativeWindowResized()"));
	m_thread->enqueue(Message(MESSAGE_WINDOW_RESIZED, window));
}

void RenderActivity::onNativeWindowRedrawNeeded (ANativeWindow* window)
{
	DE_UNREF(window);
}

void RenderActivity::onNativeWindowDestroyed (ANativeWindow* window)
{
	DBG_PRINT(("RenderActivity::onNativeWindowDestroyed()"));
	m_thread->enqueue(Message(MESSAGE_WINDOW_DESTROYED, window));
	m_thread->sync(); // Block until thread has processed all messages.
}

void RenderActivity::onInputQueueCreated (AInputQueue* queue)
{
	DBG_PRINT(("RenderActivity::onInputQueueCreated()"));
	m_thread->enqueue(Message(MESSAGE_INPUT_QUEUE_CREATED, queue));
}

void RenderActivity::onInputQueueDestroyed (AInputQueue* queue)
{
	DBG_PRINT(("RenderActivity::onInputQueueDestroyed()"));
	m_thread->enqueue(Message(MESSAGE_INPUT_QUEUE_DESTROYED, queue));
	m_thread->sync();
}

} // Android
} // tcu