#include "host_interface.h"
#include "common/assert.h"

HostInterface* g_host_interface;

HostInterface::HostInterface()
{
  Assert(!g_host_interface);
  g_host_interface = this;
}

HostInterface::~HostInterface()
{
  // system should be shut down prior to the destructor
  Assert(g_host_interface == this);
  g_host_interface = nullptr;
}
