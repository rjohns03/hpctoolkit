// -*-Mode: C++;-*- // technically C99

// * BeginRiceCopyright *****************************************************
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2016, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *

//
// Linux perf sample source interface
//


/******************************************************************************
 * system includes
 *****************************************************************************/

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/syscall.h> 
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>


/******************************************************************************
 * linux specific headers
 *****************************************************************************/
#include <linux/perf_event.h>
#include <linux/version.h>


/******************************************************************************
 * libmonitor
 *****************************************************************************/
#include <monitor.h>



/******************************************************************************
 * local includes
 *****************************************************************************/

#include "sample-sources/simple_oo.h"
#include "sample-sources/sample_source_obj.h"
#include "sample-sources/common.h"

#include <hpcrun/cct_insert_backtrace.h>
#include <hpcrun/hpcrun_stats.h>
#include <hpcrun/loadmap.h>
#include <hpcrun/messages/messages.h>
#include <hpcrun/metrics.h>
#include <hpcrun/safe-sampling.h>
#include <hpcrun/sample_event.h>
#include <hpcrun/sample_sources_registered.h>
#include <hpcrun/sample-sources/blame-shift/blame-shift.h>
#include <hpcrun/utilities/tokenize.h>
#include <hpcrun/utilities/arch/context-pc.h>

#include <evlist.h>

#include <include/linux_info.h> 

#ifdef ENABLE_PERFMON
#include "perfmon-util.h"
#endif

#include "perf-util.h" 	 // u64

//******************************************************************************
// macros
//******************************************************************************

#define DEFAULT_THRESHOLD  2000000L

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id  _sigev_un._tid
#endif

#define THREAD_SELF 	 	 0
#define CPU_ANY 	        -1
#define GROUP_FD 	        -1
#define PERF_FLAGS 		 0
#define PERF_REQUEST_0_SKID 	 2
#define PERF_WAKEUP_EACH_SAMPLE  1

#define MMAP_OFFSET_0 0

#define EXCLUDE_CALLCHAIN_USER   1


#define PERF_SIGNAL SIGIO

#define PERF_DATA_PAGE_EXP        0      // use 2^PERF_DATA_PAGE_EXP pages
#define PERF_DATA_PAGES           (1 << PERF_DATA_PAGE_EXP)  

#define BUFFER_FRONT(current_perf_mmap)              ((char *) current_perf_mmap + pagesize)
#define BUFFER_SIZE               (tail_mask + 1)
#define BUFFER_OFFSET(tail)       ((tail) & tail_mask)

#define PERF_MMAP_SIZE(pagesz)    ((pagesz) * (PERF_DATA_PAGES + 1)) 
#define PERF_TAIL_MASK(pagesz)    (((pagesz) * PERF_DATA_PAGES) - 1) 

#define PERF_EVENT_AVAILABLE_UNKNOWN 0
#define PERF_EVENT_AVAILABLE_NO      1
#define PERF_EVENT_AVAILABLE_YES     2

#define RAW_NONE        0
#define RAW_IBS_FETCH   1
#define RAW_IBS_OP      2

#define EVENT_DATA_CENTRIC "DATACENTRIC"
#define DATA_CENTRIC_ID 1
#define HPCRUN_OPTION_MULTIPLEX  "HPCRUN_MULTIPLEX"
#define HPCRUN_OPTION_PRECISE_IP "HPCRUN_PRECISE_IP"

//******************************************************************************
// type declarations
//******************************************************************************

typedef struct perf_event_header pe_header_t;

typedef struct perf_event_callchain_s {
  u64 nr;        /* number of IPs */ 
  u64 ips[];     /* vector of IPs */
} pe_callchain_t;

typedef struct perf_event_mmap_page pe_mmap_t;

typedef struct event_predefined_s {
  const char *name;
  u64 id;
} event_predefined_t;

/******************************************************************************
 * external thread-local variables
 *****************************************************************************/

extern __thread bool hpcrun_thread_suppress_sample;



//******************************************************************************
// forward declarations 
//******************************************************************************

static bool 
perf_thread_init(int event_num);

static void 
perf_thread_fini(int nevents);

static cct_node_t *
perf_add_kernel_callchain(
  cct_node_t *leaf, void *data
);

static int perf_event_handler(
  int sig, 
  siginfo_t* siginfo, 
  void* context
);


//******************************************************************************
// local variables
//******************************************************************************

static event_predefined_t events_predefined[] = {{EVENT_DATA_CENTRIC, DATA_CENTRIC_ID} };

static uint16_t perf_kernel_lm_id;

static bool perf_ksyms_avail;

static int metric_id[MAX_EVENTS];

static struct perf_event_attr events[MAX_EVENTS];

static sigset_t sig_mask;

static int pagesize;
static size_t tail_mask;

#ifndef ENABLE_PERFMON
static const char *event_name = "PERF_COUNT_HW_CPU_CYCLES";
#endif

static const char * dashes_separator = 
  "---------------------------------------------------------------------------\n";
static const char * equals_separator =
  "===========================================================================\n";

// Special case to make perf init a soft failure.
// Make sure that we don't use perf if it won't work.
static int perf_unavail = 0;

// flag if multiplex is allowed, detected, etc...
// 0: use the default multiplex
// 1: detect multiplex
// 2: do not allow multiplex
static int perf_multiplex = 0;

static int perf_precise_ip = 0;

//******************************************************************************
// thread local variables
//******************************************************************************


int                         __thread perf_thread_fd[MAX_EVENTS];
pe_mmap_t                   __thread *perf_mmap[MAX_EVENTS];


//******************************************************************************
// private operations 
//******************************************************************************

static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
	       int cpu, int group_fd, unsigned long flags)
{
   int ret;

   ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
   return ret;
}


// return the ID of the predefined event if the name matches
// return -1 otherwise
static int
getPredefinedEventID(const char *event_name)
{
  int elems = sizeof(events_predefined) / sizeof(event_predefined_t);
  
  for (int i=0; i<elems; i++) {
    const char  *event = events_predefined[i].name;
    if (strncmp(event, event_name, strlen(event)) == 0) {
      return events_predefined[i].id;
    }
  }
  return -1;
}

//----------------------------------------------------------
// stop all events
//----------------------------------------------------------
static void
perf_stop(int num_event)
{
  if (perf_thread_fd[num_event]) {
    monitor_real_pthread_sigmask(SIG_BLOCK, &sig_mask, NULL);
    // disable the counter
    ioctl(perf_thread_fd[num_event], PERF_EVENT_IOC_DISABLE, 0);
  }
}


//----------------------------------------------------------
// read from perf_events mmap'ed buffer
//----------------------------------------------------------

static int 
perf_read(
  pe_mmap_t *current_perf_mmap,
  void *buf, 
  size_t bytes_wanted
)
{
  // front of the circular data buffer
  char *data = BUFFER_FRONT(current_perf_mmap); 
  
  // compute bytes available in the circular buffer 
  size_t bytes_available = current_perf_mmap->data_head - current_perf_mmap->data_tail;

  if (bytes_wanted > bytes_available) return -1;

  // compute offset of tail in the circular buffer
  unsigned long tail = BUFFER_OFFSET(current_perf_mmap->data_tail);

  long bytes_at_right = BUFFER_SIZE - tail;

  // bytes to copy to the right of tail
  size_t right = bytes_at_right < bytes_wanted ? bytes_at_right : bytes_wanted;

  // copy bytes from tail position
  memcpy(buf, data + tail, right);

  // if necessary, wrap and continue copy from left edge of buffer
  if (bytes_wanted > right) {
    size_t left = bytes_wanted - right;
    memcpy(buf + right, data, left);
  }

  // update tail after consuming bytes_wanted
  current_perf_mmap->data_tail += bytes_wanted;

  return 0;
}


static inline int
perf_read_header(
  pe_mmap_t *current_perf_mmap,
  pe_header_t *hdr
)
{
  return perf_read(current_perf_mmap, hdr, sizeof(pe_header_t));
}


static inline int
perf_read_u64(
  pe_mmap_t *current_perf_mmap,
  u64 *val
)
{
  return perf_read(current_perf_mmap, val, sizeof(u64));
}


//----------------------------------------------------------
// predicates that test perf availability
//----------------------------------------------------------

static bool
perf_kernel_syms_avail()
{
  FILE *ksyms = fopen(LINUX_KERNEL_SYMBOL_FILE, "r");
  bool success = (ksyms != NULL);
  if (success) fclose(ksyms);
  return success;
}


static bool
perf_event_avail()
{
  static int checked = PERF_EVENT_AVAILABLE_UNKNOWN;
  
  switch (checked) {
  case PERF_EVENT_AVAILABLE_UNKNOWN:
    {
      struct stat buf;
      int rc = stat("/proc/sys/kernel/perf_event_paranoid", &buf);
      checked = (rc == 0) ? PERF_EVENT_AVAILABLE_YES : PERF_EVENT_AVAILABLE_NO; 
    }
  case PERF_EVENT_AVAILABLE_NO:      
  case PERF_EVENT_AVAILABLE_YES:     
    break;
  }

  return (checked == PERF_EVENT_AVAILABLE_YES);     
}


//----------------------------------------------------------
// initialization and finalization
//----------------------------------------------------------

static void 
perf_init()
{
  pagesize = sysconf(_SC_PAGESIZE);
  tail_mask = PERF_TAIL_MASK(pagesize);

  // initialize mask to block PERF_SIGNAL 
  sigemptyset(&sig_mask);
  sigaddset(&sig_mask, PERF_SIGNAL);

  // if kernel symbols are available, we will attempt to collect kernel 
  // callchains and add them to our call paths 
  perf_ksyms_avail = perf_kernel_syms_avail();

  if (perf_ksyms_avail) {
    hpcrun_kernel_callpath_register(perf_add_kernel_callchain);
    perf_kernel_lm_id = hpcrun_loadModule_add(LINUX_KERNEL_NAME);
  }
 
  monitor_sigaction(PERF_SIGNAL, &perf_event_handler, 0, NULL);
}


//----------------------------------------------------------
// generic initialization for event attributes
// return true if the initialization is successful,
//   false otherwise.
//----------------------------------------------------------
static int
perf_attr_init(
  const char *name,
  struct perf_event_attr *attr,
  long threshold
)
{
  // if perfmon is disabled, by default the event is cycle
  unsigned int event_code  = PERF_COUNT_HW_CPU_CYCLES;
  unsigned int event_type  = PERF_TYPE_HARDWARE;
  unsigned int sample_type = 0;

  if (strcmp(name, EVENT_DATA_CENTRIC)==0) {
    sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU | PERF_SAMPLE_TID ;
  } 
#ifdef ENABLE_PERFMON
  else if (!pfmu_getEventCode(name, &event_code)) {
     EMSG("Linux perf event not recognized: %s", name);
     return false;
  }
  event_type = pfmu_getEventType(name);
  if (event_type<0) {
     EMSG("Linux perf event type not recognized: %s", name);
     return false;
  }
#endif

  memset(attr, 0, sizeof(struct perf_event_attr));

  attr->size   = sizeof(struct perf_event_attr); /* Size of attribute structure */
  attr->type   = event_type;			 /* Type of event 		*/
  attr->config = event_code;			 /* Type-specific configuration */

  attr->sample_period = threshold;		 /* Period of sampling 		*/
  attr->precise_ip    = perf_precise_ip;	 /*  requested to have 0 skid.  */
  attr->wakeup_events = PERF_WAKEUP_EACH_SAMPLE;
  attr->disabled      = 1; 			/* the counter will be enabled later  */
  attr->sample_stack_user = 4096;

  if (perf_ksyms_avail) {
    /* Records the callchain */
    attr->sample_type 		 = sample_type | PERF_SAMPLE_CALLCHAIN;  
    attr->exclude_callchain_user = EXCLUDE_CALLCHAIN_USER;
  } else {
    attr->sample_type = PERF_SAMPLE_IP;
  }
  TMSG(LINUX_PERF, "init event %s: type: %d, code: %x", name, event_type, event_code);
  return true;
}


//----------------------------------------------------------
// allocate mmap for a given file descriptor
//----------------------------------------------------------
static pe_mmap_t*
set_mmap(int perf_fd)
{
  void *map_result = 
    mmap(NULL, PERF_MMAP_SIZE(pagesize), PROT_WRITE | PROT_READ, 
	   MAP_SHARED, perf_fd, MMAP_OFFSET_0);

  if (map_result == MAP_FAILED) {
      EMSG("Linux perf mmap failed: %s", strerror(errno));
      return NULL;
  }

  pe_mmap_t *mmap  = (pe_mmap_t *) map_result;

  if (mmap) {
    memset(mmap, 0, sizeof(pe_mmap_t));
    mmap->version = 0; 
    mmap->compat_version = 0; 
    mmap->data_head = 0; 
    mmap->data_tail = 0; 
  }
  return mmap;
}

//----------------------------------------------------------
// initialize an event
//  event_num: event number
//  name: name of event (has to be recognized by perf event)
//  threshold: sampling threshold 
//----------------------------------------------------------
static bool
perf_thread_init(int event_num)
{
  // ask sys to "create" the event
  // it returns -1 if it fails.
  perf_thread_fd[event_num] = perf_event_open(&events[event_num],
			      THREAD_SELF, CPU_ANY, 
			      GROUP_FD, PERF_FLAGS);

  TMSG(LINUX_PERF, "dbg register event %d, fd: %d", event_num, perf_thread_fd[event_num]);

  // check if perf_event_open is successful
  if (perf_thread_fd[event_num] <0 ) {
      EMSG("Linux perf event open failed: %s", strerror(errno));
      return false;
  }
  perf_mmap[event_num] = set_mmap(perf_thread_fd[event_num]);

  monitor_real_pthread_sigmask(SIG_UNBLOCK, &sig_mask, NULL);

  fcntl(perf_thread_fd[event_num], F_SETSIG, SIGIO);
  fcntl(perf_thread_fd[event_num], F_SETOWN,getpid());
  fcntl(perf_thread_fd[event_num], F_SETFL, O_RDWR |  O_ASYNC | O_NONBLOCK);

  struct f_owner_ex owner;
  owner.type = F_OWNER_TID;
  owner.pid  = syscall(SYS_gettid);
  int ret = fcntl(perf_thread_fd[event_num], F_SETOWN_EX, &owner);

  if (ret == -1) {
    TMSG(LINUX_PERF, "can't set fcntl(F_SETOWN_EX) on %d: %s\n", 
	    perf_thread_fd[event_num], strerror(errno));
  } else {
    ioctl(perf_thread_fd[event_num], PERF_EVENT_IOC_RESET, 0);
  }
  return (ret >= 0);
}


//----------------------------------------------------------
// actions when the program terminates: 
//  - unmap the memory
//  - close file descriptors used by each event
//----------------------------------------------------------
void
perf_thread_fini(int nevents)
{
  if (perf_thread_fd[0]) {
    int i;
    
    for (i=0; i< nevents; i++) {
      close(perf_thread_fd[i]);
      munmap(perf_mmap[i], PERF_MMAP_SIZE(pagesize));
    }
  }
}


//----------------------------------------------------------
// special mmap buffer reading for PERF_SAMPLE_READ
//----------------------------------------------------------
static void
handle_struct_read_format( pe_mmap_t *perf_mmap, int read_format) 
{
  u64 value, id, nr, time_enabled, time_running;

  if (read_format & PERF_FORMAT_GROUP) {
    perf_read_u64(perf_mmap, &nr);
  } else {
    perf_read_u64(perf_mmap, &value);
  }
  
  if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED) {
    perf_read_u64(perf_mmap, &time_enabled);
  }
  if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING) {
    perf_read_u64(perf_mmap, &time_running);
  }

  if (read_format & PERF_FORMAT_GROUP) {
    for(int i=0;i<nr;i++) {
      perf_read_u64(perf_mmap, &value);

      if (read_format & PERF_FORMAT_ID) {
        perf_read_u64(perf_mmap, &id);
      }
    }
  }
  else {
    if (read_format & PERF_FORMAT_ID) {
      perf_read_u64(perf_mmap, &id);
    }
  }
}



//----------------------------------------------------------
// signal handling and processing of kernel callchains
//----------------------------------------------------------

static cct_node_t *
perf_sample_callchain(pe_mmap_t *current_perf_mmap, cct_node_t *leaf)
{
  cct_node_t *parent = leaf;	// parent of the cct
  u64 n_frames; 		// check of sample type

  // determine how many frames in the call chain 
  if (perf_read_u64( current_perf_mmap, &n_frames) == 0) {
    if (n_frames > 0) {
      // allocate space to receive IPs for kernel callchain 
      u64 *ips = alloca(n_frames * sizeof(u64));

      // read the IPs for the frames 
      if (perf_read( current_perf_mmap, ips, n_frames * sizeof(u64)) == 0) {

	// add kernel IPs to the call chain top down, which is the 
	// reverse of the order in which they appear in ips
	for (int i = n_frames - 1; i > 0; i--) {
	  ip_normalized_t npc = 
		{ .lm_id = perf_kernel_lm_id, .lm_ip = ips[i] };
	  cct_addr_t frm = { .ip_norm = npc };
	  cct_node_t *child = hpcrun_cct_insert_addr(parent, &frm);
	  parent = child;
	}
      } else {
	TMSG(LINUX_PERF, "unable to read all %d frames", n_frames );
      }
    }
  } else {
    TMSG(LINUX_PERF, "unable to read the number of frames" );
  }
  return parent;
}


//----------------------------------------------------------
// extend a user-mode callchain with kernel frames (if any)
//----------------------------------------------------------
static cct_node_t *
perf_add_kernel_callchain(
  cct_node_t *leaf, void *data_aux
)
{
  cct_node_t *parent = leaf;
  pe_header_t hdr; 
  int current_event_index = *((int*) data_aux);
  pe_mmap_t *current_perf_mmap = perf_mmap[current_event_index];

  if (perf_read_header(current_perf_mmap, &hdr) == 0) {
    if (hdr.type == PERF_RECORD_SAMPLE) {
      if (hdr.size <= 0) {
	return parent;
      }
      struct perf_event_attr *current_event = &(events[current_event_index]);
      int sample_type = current_event->sample_type;

      if (sample_type & PERF_SAMPLE_IDENTIFIER) {
      }
      if (sample_type & PERF_SAMPLE_IP) {
	u64 ip;
	perf_read_u64(current_perf_mmap, &ip);
      }
      if (sample_type & PERF_SAMPLE_TID) {
      }
      if (sample_type & PERF_SAMPLE_TIME) {
      }
      if (sample_type & PERF_SAMPLE_ADDR) {
	u64 addr;
	perf_read_u64(current_perf_mmap, &addr);
      }
      if (sample_type & PERF_SAMPLE_ID) {
      }
      if (sample_type & PERF_SAMPLE_STREAM_ID) {
      }
      if (sample_type & PERF_SAMPLE_CPU) {
      }
      if (sample_type & PERF_SAMPLE_PERIOD) {
      }
      if (sample_type & PERF_SAMPLE_READ) {
	handle_struct_read_format(current_perf_mmap,
			 current_event->read_format);
      }
      if (sample_type & PERF_SAMPLE_CALLCHAIN) {
	// add call chain from the kernel
 	parent = perf_sample_callchain(current_perf_mmap, parent);
      }
      if (sample_type & PERF_SAMPLE_RAW) {
      }
      if (sample_type & PERF_SAMPLE_BRANCH_STACK) {
      }
      if (sample_type & PERF_SAMPLE_REGS_USER) {
      }
      if (sample_type & PERF_SAMPLE_STACK_USER) {
      }
      if (sample_type & PERF_SAMPLE_WEIGHT) {
      }
      if (sample_type & PERF_SAMPLE_DATA_SRC) {
      }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,13,0)
      // only available since kernel 3.19
      if (sample_type & PERF_SAMPLE_TRANSACTION) {
      }
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,19,0)
      // only available since kernel 3.19
      if (sample_type & PERF_SAMPLE_REGS_INTR) {
      }
#endif
    }
  }

  return parent;
}


// ---------------------------------------------
// create a metric and return the descriptor of the new metric
// ---------------------------------------------

static metric_desc_t*
create_metric(sample_source_t *self, int index, MetricFlags_ValFmt_t type,
	char *name, long threshold, metric_desc_properties_t prop)
{
  // set the metric for this perf event
  metric_id[index] = hpcrun_new_metric();

  metric_desc_t *m = hpcrun_set_metric_info_and_period(metric_id[index], name,
				    type, threshold, prop);
  if (m == NULL) {
    EMSG("Error: unable to create metric #%d: %s", index, name);
  }
  return m;
}

// ---------------------------------------------
// get the index of the file descriptor
// ---------------------------------------------

static int
get_fd_index(int num_events, int fd)
{
  int i;
  
  for (i=0; i<num_events; i++) {
    if (perf_thread_fd[i] == fd)
      return i;
  } 
  // file descriptor not recognized
  return -1; 
}


static long
getEnvLong(const char *env_var)
{
  const char *precise_ip = getenv(env_var);

  if (precise_ip) {
    char *end_ptr;
    long val = strtol( precise_ip, &end_ptr, 10 );
    if ( end_ptr != env_var && (val < LONG_MAX && val > LONG_MIN) ) {
      return val;
    }
  }
  return 0;
}

/******************************************************************************
 * method functions
 *****************************************************************************/

// --------------------------------------------------------------------------
// event occurs when the sample source is initialized
// --------------------------------------------------------------------------
static void
METHOD_FN(init)
{
  TMSG(LINUX_PERF, "%d: init", self->sel_idx);

  perf_unavail = ( !perf_event_avail() );

  // checking the option of multiplexing:
  // the env variable is set by hpcrun or by user (case for static exec)
  perf_multiplex  = getEnvLong( HPCRUN_OPTION_MULTIPLEX );

  perf_precise_ip = getEnvLong( HPCRUN_OPTION_PRECISE_IP );

  self->state = INIT;
  TMSG(LINUX_PERF, "%d: init OK", self->sel_idx);
}


// --------------------------------------------------------------------------
// when a new thread is created
// --------------------------------------------------------------------------
static void
METHOD_FN(thread_init)
{
  TMSG(LINUX_PERF, "%d: thread init", self->sel_idx);
  if (perf_unavail) { return; }

  TMSG(LINUX_PERF, "%d: thread init OK", self->sel_idx);
}


// --------------------------------------------------------------------------
// start of the thread
// --------------------------------------------------------------------------
static void
METHOD_FN(thread_init_action)
{
  TMSG(LINUX_PERF, "%d: thread init action", self->sel_idx);

  if (perf_unavail) { return; }

  TMSG(LINUX_PERF, "%d: thread init action OK", self->sel_idx);
}


// --------------------------------------------------------------------------
// start of the application 
// --------------------------------------------------------------------------
static void
METHOD_FN(start)
{
  TMSG(LINUX_PERF, "%d: start", self->sel_idx);

  if (perf_unavail) { 
    return; 
  }

  source_state_t my_state = TD_GET(ss_state)[self->sel_idx];

  // make LINUX_PERF start idempotent.  the application can turn on sampling
  // anywhere via the start-stop interface, so we can't control what
  // state LINUX_PERF is in.

  if (my_state == START) {
    TMSG(LINUX_PERF,"%d: *NOTE* LINUX_PERF start called when already in state START",
		     self->sel_idx);
    return;
  }

  // setup all requested events
  // if an event cannot be initialized, we still keep it in our list
  //  but there will be no samples

  int nevents = (self->evl).nevents; 

  for (int i=0; i<nevents; i++) {

    // initialize this event. If it's valid, we set the metric for the event
    if (perf_thread_init(i)) { 
      ioctl(perf_thread_fd[i], PERF_EVENT_IOC_ENABLE, 0);
    }
  }

  thread_data_t* td = hpcrun_get_thread_data();
  td->ss_state[self->sel_idx] = START;

  TMSG(LINUX_PERF, "%d: start OK", self->sel_idx);
}

// --------------------------------------------------------------------------
// end of thread
// --------------------------------------------------------------------------
static void
METHOD_FN(thread_fini_action)
{
  TMSG(LINUX_PERF, "%d: unregister thread", self->sel_idx);
  if (perf_unavail) { return; }

  TMSG(LINUX_PERF, "%d: unregister thread OK", self->sel_idx);
}


// --------------------------------------------------------------------------
// end of the application
// --------------------------------------------------------------------------
static void
METHOD_FN(stop)
{
  TMSG(LINUX_PERF, "%d: stop", self->sel_idx);

  if (perf_unavail) return; 

  source_state_t my_state = TD_GET(ss_state)[self->sel_idx];
  if (my_state == STOP) {
    TMSG(LINUX_PERF,"%d: *NOTE* PERF stop called when already in state STOP",
	 	    self->sel_idx);
    return;
  }

  if (my_state != START) {
    TMSG(LINUX_PERF,"%d: *WARNING* PERF stop called when not in state START",
		     self->sel_idx);
    return;
  }

  int nevents = (self->evl).nevents; 
  for (int i=0; i<nevents; i++)
  {
    perf_stop(i);
  }
  thread_data_t* td = hpcrun_get_thread_data();
  td->ss_state[self->sel_idx] = STOP;

  TMSG(LINUX_PERF, "%d: stop OK", self->sel_idx);
}

// --------------------------------------------------------------------------
// really end
// --------------------------------------------------------------------------
static void
METHOD_FN(shutdown)
{
  TMSG(LINUX_PERF, "shutdown");

  if (perf_unavail) { return; }

  METHOD_CALL(self, stop); // make sure stop has been called
  // FIXME: add component shutdown code here

  int nevents = (self->evl).nevents; 
  perf_thread_fini(nevents);

#ifdef ENABLE_PERFMON
  // terminate perfmon
  pfmu_fini();
#endif

  self->state = UNINIT;
  TMSG(LINUX_PERF, "shutdown OK");
}


// --------------------------------------------------------------------------
// Return true if Linux perf recognizes the name, whether supported or not.
// We'll handle unsupported events later.
// --------------------------------------------------------------------------
static bool
METHOD_FN(supports_event, const char *ev_str)
{
  TMSG(LINUX_PERF, "supports event %s", ev_str);

#ifdef ENABLE_PERFMON
  // perfmon is smart enough to detect if pfmu has been initialized or not
  pfmu_init();
#endif

  if (perf_unavail) { return false; }

  if (self->state == UNINIT){
    METHOD_CALL(self, init);
  }

  // first, check if the event is a predefined event
  bool ret = getPredefinedEventID(ev_str) >= 0;

  // this is not a predefined event, we need to consult to perfmon (if enabled)
  if (!ret) {
#ifdef ENABLE_PERFMON
    long thresh;
    char ev_tmp[1024];
    if (! hpcrun_extract_ev_thresh(ev_str, sizeof(ev_tmp), ev_tmp, &thresh, DEFAULT_THRESHOLD)) {
      AMSG("WARNING: %s using default threshold %ld, "
   	"better to use an explicit threshold.", ev_str, DEFAULT_THRESHOLD);
    }
    ret = pfmu_isSupported(ev_tmp);
#else
    ret = (strncmp(event_name, ev_str, strlen(event_name)) == 0); 
#endif
  }

  TMSG(LINUX_PERF, "supports event %s", (ret? "OK" : "FAIL"));
  return ret;
}

 
// --------------------------------------------------------------------------
// handle a list of events
// --------------------------------------------------------------------------
static void
METHOD_FN(process_event_list, int lush_metrics)
{
  TMSG(LINUX_PERF, "process event list");
  if (perf_unavail) { return; }

  metric_desc_properties_t prop = metric_property_none;
  char *event;

  perf_init();

  char *evlist = METHOD_CALL(self, get_event_str);
  int num_events = 0;

  // TODO: stupid way to count the number of events

  for (event = start_tok(evlist); more_tok(); event = next_tok()) {
    num_events++;
  }
  // manually, setup the number of events. In theory, this is to be done
  //  automatically. But in practice, it didn't. Not sure why.
  self->evl.nevents = num_events;

  
  // setup all requested events
  // if an event cannot be initialized, we still keep it in our list
  //  but there will be no samples
  int i=0;
  for (event = start_tok(evlist); more_tok(); event = next_tok(), i++) {
    char name[1024];
    long threshold;

    TMSG(LINUX_PERF,"checking event spec = %s",event);

    hpcrun_extract_ev_thresh(event, sizeof(name), name, &threshold, 
			     DEFAULT_THRESHOLD);

    // initialize the event attributes
    struct perf_event_attr *attr = &events[i];
    perf_attr_init(name, attr, threshold);

    // initialize the property of the metric
    // if the metric's name has "CYCLES" it mostly a cycle metric 
    //  this assumption is not true, but it's quite closed

    prop = (strstr(name, "CYCLES") != NULL) ? metric_property_cycles : metric_property_none;

    char *name_dup = strdup(name); // we need to duplicate the name of the metric until the end
				   // since the OS will free it, we don't have to do it in hpcrun
    struct metric_desc_t *m = create_metric(self, i, MetricFlags_ValFmt_Int, name_dup, threshold, prop);

    // detecting multiplex and compute the scale factor
    if (perf_multiplex == 1) {
      const size_t MAX_LABEL_CHARS = 80;

      // the formula for the estimate count = raw_count * scale_factor
      // 				    = metric(i) * metric(i+1)
      if (m != NULL) {
      	char buffer[MAX_LABEL_CHARS]; 
      	sprintf(buffer, "($%d*$%d)", i, i+1);
        m -> formula = buffer;
      }

      // create metric for application's scale factor (i.e  time enabled/time running)
      char *scale_factor_metric = (char *) hpcrun_malloc(sizeof (char)*MAX_LABEL_CHARS);
      snprintf(scale_factor_metric, MAX_LABEL_CHARS, "SF-%s", name ); // SF: Scale factor

      i++;
      create_metric(self, i, MetricFlags_ValFmt_Real, scale_factor_metric, 1, metric_property_none);
    }
  }
}


// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
static void
METHOD_FN(gen_event_set, int lush_metrics)
{
}


// --------------------------------------------------------------------------
// list events
// --------------------------------------------------------------------------
static void
METHOD_FN(display_events)
{
  if (perf_event_avail()) {
    printf(equals_separator);
    printf("Available Linux perf events\n");
    printf(equals_separator);

#ifdef ENABLE_PERFMON
    // perfmon is smart enough to detect if pfmu has been initialized or not
    pfmu_init();
    printf(dashes_separator);
    pfmu_showEventList();
    pfmu_fini();
#else
    printf("Name\t\tDescription\n");
    printf(dashes_separator);

    printf("%s\tTotal cycles.\n", 
	   "PERF_COUNT_HW_CPU_CYCLES");
    printf("\n");
#endif
  }
}



// ------------------------------------------------------------
// Disable perf event
// ------------------------------------------------------------
static void
disable_perf_event(int fd)
{
#if 0
  int rc = (0>ioctl(fd, PERF_EVENT_IOC_DISABLE, 0));
  if (rc) {
    TMSG(LINUX_PERF, "error fd %d in IOC_DISABLE", fd);
  }
#endif
}

// ------------------------------------------------------------
// Refresh a disabled perf event
// ------------------------------------------------------------

static void 
restart_perf_event(int fd)
{
#if 0
  int rc = ioctl(fd, PERF_EVENT_IOC_REFRESH, 1);
  if (rc == -1) {
    TMSG(LINUX_PERF, "error fd %d in IOC_REFRESH", fd);
  }
#endif
}
/***************************************************************************
 * object
 ***************************************************************************/

#define ss_name linux_perf
#define ss_cls SS_HARDWARE

#include "sample-sources/ss_obj.h"

// ---------------------------------------------
// signal handler
// ---------------------------------------------

static int
perf_event_handler(
  int sig, 
  siginfo_t* siginfo, 
  void* context
)
{
  if (siginfo->si_code < 0) {
    // signal not generated by kernel for profiling
    TMSG(LINUX_PERF, "signal si_code %d < 0 indicates not from kernel", 
	 siginfo->si_code);
    return 1; // tell monitor the signal has not been handled.
  }

  // first, we need to disable the event
  int fd = siginfo->si_fd;
  disable_perf_event(fd);

  // if the interrupt came while inside our code, then drop the sample
  // and return and avoid the potential for deadlock.

  void *pc = hpcrun_context_pc(context); 

  if (! hpcrun_safe_enter_async(pc)) {
    hpcrun_stats_num_samples_blocked_async_inc();
    restart_perf_event(fd);
    return 0; // tell monitor the signal has been handled.
  }

  // get the index of the file descriptor (if we have multiple events)
  // if the file descriptor is not on the list, we shouldn't store the 
  // metrics. Perhaps we should throw away?

  sample_source_t *self = &obj_name();

  int nevents = self->evl.nevents;
  int index   = get_fd_index(nevents, fd);

  // store the metric if the metric index is correct
  if ( index >= 0 ) {
    sample_val_t sv = hpcrun_sample_callpath(context, metric_id[index], 1,
					   0/*skipInner*/, 0/*isSync*/, (void*) &index);

    // check if we have multiplexing or not with this counter
    // if the time enabled is not the same as running time, then it's multiplexed
    //metric_desc_t* metric = hpcrun_id2metric(metric_id[index]);

    if (perf_multiplex == 1) {
      // record time enabled and time running
      // if the time enabled is not the same as running time, then it's multiplexed
      
      u64 time_enabled = perf_mmap[index]->time_enabled;
      u64 time_running = perf_mmap[index]->time_running;

      cct_node_t *node = sv.sample_node;
      cct_metric_data_set(metric_id[index+1], node, 
        (cct_metric_data_t){.r =  (double) time_enabled / time_running });
    }
#if 0
    if (info->time_enabled != info->time_running) {
      TMSG(LINUX_PERF, "Multiplex: enabled: %d, running: %d ", 
	info->time_enabled, info->time_running);
    }
#endif
    blame_shift_apply(metric_id[index], sv.sample_node, 1 /*metricIncr*/);
  } else {
    // signal not from perf event
    TMSG(LINUX_PERF, "signal si_code %d with fd %d: unknown perf event", 
	 siginfo->si_code, fd);
    restart_perf_event(fd);
    return 1; // tell monitor the signal has not been handled.
  }

  hpcrun_safe_exit();

  // restart the event 
  restart_perf_event(fd);

  return 0; // tell monitor the signal has been handled.
}
