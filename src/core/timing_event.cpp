#include "timing_event.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "system.h"
Log_SetChannel(TimingEvents);

namespace TimingEvents {

static TimingEvent* s_active_events_head;
static TimingEvent* s_active_events_tail;
static TimingEvent* s_current_event = nullptr;
static u32 s_active_event_count = 0;
static u64 s_global_tick_counter = 0;

u64 GetGlobalTickCounter()
{
  return s_global_tick_counter + CPU::GetPendingTicks();
}

void Initialize()
{
  Reset();
}

void Reset()
{
  const u64 old_ts = s_global_tick_counter;
  s_global_tick_counter = 0;

  for (TimingEvent* event = s_active_events_head; event; event = event->next)
  {
    event->m_next_run_time = event->m_next_run_time - old_ts + s_global_tick_counter;
    event->m_last_run_time = old_ts - event->m_last_run_time + s_global_tick_counter;
  }
}

void Shutdown()
{
  Assert(s_active_event_count == 0);
}

std::unique_ptr<TimingEvent> CreateTimingEvent(std::string name, TickCount period, TickCount interval,
                                               TimingEventCallback callback, bool activate)
{
  std::unique_ptr<TimingEvent> event =
    std::make_unique<TimingEvent>(std::move(name), period, interval, std::move(callback));
  if (activate)
    event->Activate();

  return event;
}

void UpdateCPUDowncount()
{
  if (!CPU::g_state.frame_done)
  {
    const u64 gtc = GetGlobalTickCounter();
    const u64 next_event_run_time = s_active_events_head->m_next_run_time;
    CPU::g_state.downcount = (next_event_run_time > gtc) ? static_cast<u32>(next_event_run_time - gtc) : 0u;
  }
}

static void SortEvent(TimingEvent* event)
{
  const u64 event_downcount = event->m_next_run_time;

  if (event->prev && event->prev->m_next_run_time > event_downcount)
  {
    // move backwards
    TimingEvent* current = event->prev;
    while (current && current->m_next_run_time > event_downcount)
      current = current->prev;

    // unlink
    if (event->prev)
      event->prev->next = event->next;
    else
      s_active_events_head = event->next;
    if (event->next)
      event->next->prev = event->prev;
    else
      s_active_events_tail = event->prev;

    // insert after current
    if (current)
    {
      event->next = current->next;
      if (current->next)
        current->next->prev = event;
      else
        s_active_events_tail = event;

      event->prev = current;
      current->next = event;
    }
    else
    {
      // insert at front
      DebugAssert(s_active_events_head);
      s_active_events_head->prev = event;
      event->prev = nullptr;
      event->next = s_active_events_head;
      s_active_events_head = event;
      UpdateCPUDowncount();
    }
  }
  else if (event->next && event_downcount > event->next->m_next_run_time)
  {
    // move forwards
    TimingEvent* current = event->next;
    while (current && event_downcount > current->m_next_run_time)
      current = current->next;

    // unlink
    if (event->prev)
      event->prev->next = event->next;
    else
      s_active_events_head = event->next;
    if (event->next)
      event->next->prev = event->prev;
    else
      s_active_events_tail = event->prev;

    // insert before current
    if (current)
    {
      event->next = current;
      event->prev = current->prev;

      if (current->prev)
        current->prev->next = event;
      else
        s_active_events_head = event;

      current->prev = event;
    }
    else
    {
      // insert at back
      DebugAssert(s_active_events_tail);
      s_active_events_tail->next = event;
      event->next = nullptr;
      event->prev = s_active_events_tail;
      s_active_events_tail = event;
    }
  }
}

static void AddActiveEvent(TimingEvent* event)
{
  DebugAssert(!event->prev && !event->next);
  s_active_event_count++;

  TimingEvent* current = nullptr;
  TimingEvent* next = s_active_events_head;
  while (next && event->m_next_run_time > next->m_next_run_time)
  {
    current = next;
    next = next->next;
  }

  if (!next)
  {
    // new tail
    event->prev = s_active_events_tail;
    if (s_active_events_tail)
    {
      s_active_events_tail->next = event;
      s_active_events_tail = event;
    }
    else
    {
      // first event
      s_active_events_tail = event;
      s_active_events_head = event;
      UpdateCPUDowncount();
    }
  }
  else if (!current)
  {
    // new head
    event->next = s_active_events_head;
    s_active_events_head->prev = event;
    s_active_events_head = event;
    UpdateCPUDowncount();
  }
  else
  {
    // inbetween current < event > next
    event->prev = current;
    event->next = next;
    current->next = event;
    next->prev = event;
  }
}

static void RemoveActiveEvent(TimingEvent* event)
{
  DebugAssert(s_active_event_count > 0);

  if (event->next)
  {
    event->next->prev = event->prev;
  }
  else
  {
    s_active_events_tail = event->prev;
  }

  if (event->prev)
  {
    event->prev->next = event->next;
  }
  else
  {
    s_active_events_head = event->next;
    if (s_active_events_head)
      UpdateCPUDowncount();
  }

  event->prev = nullptr;
  event->next = nullptr;

  s_active_event_count--;
}

static void SortEvents()
{
  std::vector<TimingEvent*> events;
  events.reserve(s_active_event_count);

  TimingEvent* next = s_active_events_head;
  while (next)
  {
    TimingEvent* current = next;
    events.push_back(current);
    next = current->next;
    current->prev = nullptr;
    current->next = nullptr;
  }

  s_active_events_head = nullptr;
  s_active_events_tail = nullptr;
  s_active_event_count = 0;

  for (TimingEvent* event : events)
    AddActiveEvent(event);
}

static TimingEvent* FindActiveEvent(const char* name)
{
  for (TimingEvent* event = s_active_events_head; event; event = event->next)
  {
    if (event->GetName().compare(name) == 0)
      return event;
  }

  return nullptr;
}

void RunEvents()
{
  DebugAssert(!s_current_event);

  u32 pending_ticks = CPU::GetPendingTicks();
  CPU::ResetPendingTicks();
  while (pending_ticks > 0)
  {
    const u32 time =
      std::min(pending_ticks, static_cast<u32>(s_active_events_head->m_next_run_time - s_global_tick_counter));
    s_global_tick_counter += time;
    pending_ticks -= time;

    // Now we can actually run the callbacks.
    const u64 gtc = s_global_tick_counter;
    while (gtc >= s_active_events_head->m_next_run_time)
    {
      // move it to the end, since that'll likely be its new position
      TimingEvent* event = s_active_events_head;
      s_current_event = event;

      // Factor late time into the time for the next invocation.
      const TickCount ticks_late = static_cast<TickCount>(gtc - event->m_next_run_time);
      const TickCount ticks_to_execute = static_cast<TickCount>(gtc - event->m_last_run_time);
      event->m_next_run_time = gtc + static_cast<u32>(event->m_interval);
      event->m_last_run_time = gtc;

      // The cycles_late is only an indicator, it doesn't modify the cycles to execute.
      event->m_callback(ticks_to_execute, ticks_late);
      if (event->m_active)
        SortEvent(event);
    }
  }

  s_current_event = nullptr;
  UpdateCPUDowncount();
}

bool DoState(StateWrapper& sw)
{
  sw.Do(&s_global_tick_counter);

  if (sw.IsReading())
  {
    // Load timestamps for the clock events.
    // Any oneshot events should be recreated by the load state method, so we can fix up their times here.
    u32 event_count = 0;
    sw.Do(&event_count);

    for (u32 i = 0; i < event_count; i++)
    {
      std::string event_name;
      u64 next_run_time, last_run_time;
      TickCount period, interval;
      sw.Do(&event_name);
      sw.Do(&next_run_time);
      sw.Do(&last_run_time);
      sw.Do(&period);
      sw.Do(&interval);
      if (sw.HasError())
        return false;

      TimingEvent* event = FindActiveEvent(event_name.c_str());
      if (!event)
      {
        Log_WarningPrintf("Save state has event '%s', but couldn't find this event when loading.", event_name.c_str());
        continue;
      }

      // Using reschedule is safe here since we call sort afterwards.
      event->m_next_run_time = next_run_time;
      event->m_last_run_time = last_run_time;
      event->m_period = period;
      event->m_interval = interval;
    }

    Log_DevPrintf("Loaded %u events from save state.", event_count);
    SortEvents();
  }
  else
  {

    sw.Do(&s_active_event_count);

    for (TimingEvent* event = s_active_events_head; event; event = event->next)
    {
      sw.Do(&event->m_name);
      sw.Do(&event->m_next_run_time);
      sw.Do(&event->m_last_run_time);
      sw.Do(&event->m_period);
      sw.Do(&event->m_interval);
    }

    Log_DevPrintf("Wrote %u events to save state.", s_active_event_count);
  }

  return !sw.HasError();
}

} // namespace TimingEvents

TimingEvent::TimingEvent(std::string name, TickCount period, TickCount interval, TimingEventCallback callback)
  : m_period(period), m_interval(interval), m_callback(std::move(callback)), m_name(std::move(name)), m_active(false)
{
  const u64 gtc = TimingEvents::GetGlobalTickCounter();
  m_next_run_time = gtc + static_cast<u32>(interval);
  m_last_run_time = gtc;
}

TimingEvent::~TimingEvent()
{
  if (m_active)
    TimingEvents::RemoveActiveEvent(this);
}

TickCount TimingEvent::GetTicksSinceLastExecution() const
{
  return static_cast<TickCount>(TimingEvents::GetGlobalTickCounter() - m_last_run_time);
}

TickCount TimingEvent::GetTicksUntilNextExecution() const
{
  const u64 gtc = TimingEvents::GetGlobalTickCounter();
  return (gtc >= m_next_run_time) ? 0 : static_cast<TickCount>(m_next_run_time - gtc);
}

void TimingEvent::Schedule(TickCount ticks)
{
  const u64 gtc = TimingEvents::GetGlobalTickCounter();
  m_next_run_time = gtc + static_cast<u32>(ticks);

  if (!m_active)
  {
    // Event is going active, so we want it to only execute ticks from the current timestamp.
    m_last_run_time = gtc;
    m_active = true;
    TimingEvents::AddActiveEvent(this);
  }
  else
  {
    // Event is already active, so we leave the time since last run alone, and just modify the downcount.
    // If this is a call from an IO handler for example, re-sort the event queue.
    if (TimingEvents::s_current_event != this)
      TimingEvents::SortEvent(this);
  }
}

void TimingEvent::SetIntervalAndSchedule(TickCount ticks)
{
  SetInterval(ticks);
  Schedule(ticks);
}

void TimingEvent::SetPeriodAndSchedule(TickCount ticks)
{
  SetPeriod(ticks);
  SetInterval(ticks);
  Schedule(ticks);
}

void TimingEvent::Reset()
{
  if (!m_active)
    return;

  const u64 gtc = TimingEvents::GetGlobalTickCounter();
  m_next_run_time = gtc + static_cast<u32>(m_interval);
  m_last_run_time = 0;
  if (TimingEvents::s_current_event != this)
    TimingEvents::SortEvent(this);
}

void TimingEvent::InvokeEarly(bool force /* = false */)
{
  if (!m_active)
    return;

  const u64 gtc = TimingEvents::GetGlobalTickCounter();
  const TickCount ticks_to_execute = (gtc >= m_last_run_time) ? static_cast<TickCount>(gtc - m_last_run_time) : 0;
  if (!force && ticks_to_execute < m_period)
    return;

  m_next_run_time = gtc + static_cast<u32>(m_interval);
  m_last_run_time = gtc;
  m_callback(ticks_to_execute, 0);

  // Since we've changed the downcount, we need to re-sort the events.
  DebugAssert(TimingEvents::s_current_event != this);
  TimingEvents::SortEvent(this);
}

void TimingEvent::Activate()
{
  if (m_active)
    return;

  // leave the downcount intact
  const u64 gtc = TimingEvents::GetGlobalTickCounter();
  m_next_run_time = gtc + static_cast<u32>(m_interval);
  m_last_run_time = gtc;

  m_active = true;
  TimingEvents::AddActiveEvent(this);
}

void TimingEvent::Deactivate()
{
  if (!m_active)
    return;

  m_active = false;
  TimingEvents::RemoveActiveEvent(this);
}
