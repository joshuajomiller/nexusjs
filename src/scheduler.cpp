/*
 * Nexus.js - The next-gen JavaScript platform
 * Copyright (C) 2016  Abdullah A. Hassan <abdullah@webtomizer.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "scheduler.h"
#include "nexus.h"
#include "task.h"

#include <functional>

NX::Scheduler::Scheduler (NX::Nexus * nexus, unsigned int maxThreads):
  myNexus(nexus), myMaxThreads(maxThreads), myThreadCount(0), myService(), myWork(),
  myThreadGroup(), myCurrentTask(), myTaskQueue(256), myTaskCount(0), myActiveTaskCount(0),
  myPauseTasks(false)
{
  myService.reset(new boost::asio::io_service(maxThreads));
  myService->stop();
}

NX::Scheduler::~Scheduler()
{
  this->stop();
  myThreadGroup.join_all();
}

void NX::Scheduler::addThread()
{
  myThreadGroup.add_thread(new boost::thread(boost::bind(&NX::Scheduler::dispatcher, this)));
}

void NX::Scheduler::dispatcher()
{
  myThreadCount++;
  while (myService->poll_one() || remaining())
  {
    if (!processTasks())
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    std::this_thread::yield();
    if (myService->stopped())
      break;
  }
  myThreadCount--;
}

void NX::Scheduler::makeCurrent (NX::AbstractTask * task)
{
  myCurrentTask.reset(task);
}

bool NX::Scheduler::processTasks()
{
  if (myPauseTasks) return false;
  NX::AbstractTask * task = nullptr;
  if (myTaskQueue.pop(task))
  {
    myActiveTaskCount++;
    myCurrentTask.reset(task);
    if (myCurrentTask->status() != NX::AbstractTask::ABORTED) {
      if (myCurrentTask.get() && myCurrentTask->status() == NX::AbstractTask::INACTIVE)
        myCurrentTask->create();
      if (myCurrentTask.get() &&(myCurrentTask->status() == NX::AbstractTask::CREATED ||
                                 myCurrentTask->status() == NX::AbstractTask::PENDING))
        myCurrentTask->enter();
      if (myCurrentTask.get() && myCurrentTask->status() == NX::AbstractTask::PENDING)
      {
        myTaskQueue.push(myCurrentTask.release());
      }
      else if (myCurrentTask.get()) {
        myCurrentTask->exit();
        myCurrentTask.reset();
      }
    }
    myActiveTaskCount--;
    return true;
  }
  else
    return false;
}

void NX::Scheduler::balanceThreads()
{
  while(myTaskCount > myThreadGroup.size() && myThreadGroup.size() < myMaxThreads)
    addThread();
}

void NX::Scheduler::start()
{
  BOOST_ASSERT_MSG(myService->stopped(), "call to start with a service already running");
  myService->reset();
  myWork.reset(new boost::asio::io_service::work(*myService));
  balanceThreads();
}

void NX::Scheduler::stop()
{
  myWork.reset();
  myService->stop();
}

void NX::Scheduler::join()
{
  myThreadGroup.join_all();
}

NX::AbstractTask * NX::Scheduler::scheduleAbstractTask (NX::AbstractTask * task)
{
  myTaskQueue.push(task);
  balanceThreads();
  return task;
}

NX::Task * NX::Scheduler::scheduleTask (const NX::Scheduler::CompletionHandler & handler)
{
  NX::Task * task = new NX::Task(handler, this);
  scheduleAbstractTask(task);
  return task;
}

NX::Task * NX::Scheduler::scheduleTask (const NX::Scheduler::duration & time, const NX::Scheduler::CompletionHandler & handler)
{
  NX::Task * taskObject = new NX::Task(handler, this);
  std::shared_ptr<timer_type> timer(new timer_type(*myService));
  timer->expires_from_now(time);
  timer->async_wait(boost::bind(boost::bind(&Scheduler::scheduleAbstractTask, this, taskObject), timer));
  taskObject->addCancellationHandler([=]() { timer->cancel(); });
  return taskObject;
}

NX::CoroutineTask * NX::Scheduler::scheduleCoroutine (const NX::Scheduler::CompletionHandler & handler)
{
  NX::CoroutineTask * task = new NX::CoroutineTask(handler, this);
  scheduleAbstractTask(task);
  return task;
}

NX::CoroutineTask * NX::Scheduler::scheduleCoroutine (const NX::Scheduler::duration & time, const NX::Scheduler::CompletionHandler & handler)
{
  NX::CoroutineTask * taskObject = new NX::CoroutineTask(handler, this);
  std::shared_ptr<timer_type> timer(new timer_type(*myService));
  timer->expires_from_now(time);
  timer->async_wait(boost::bind(boost::bind(&Scheduler::scheduleAbstractTask, this, taskObject), timer));
  taskObject->addCancellationHandler([=]() { timer->cancel(); });
  return taskObject;
}

void NX::Scheduler::yield()
{
  if (!myCurrentTask.get()) {
    throw std::runtime_error("call to yield outside of a scheduler task");
  }
  if (!dynamic_cast<NX::CoroutineTask*>(myCurrentTask.get())) {
    throw std::runtime_error("call to yield outside of a coroutine");
  }
  myCurrentTask->yield();
}
