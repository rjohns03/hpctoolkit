// -*-Mode: C++;-*- // technically C99

#ifndef _hpctoolkit_blame_map_h_
#define _hpctoolkit_blame_map_h_

// * BeginRiceCopyright *****************************************************
//
// $HeadURL: $
// $Id$
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2013, Rice University
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

//******************************************************************************
//
// map for recording directed blame for locks, critical sections, ...
//
//******************************************************************************


/******************************************************************************
 * system includes
 *****************************************************************************/

#include <stdint.h>



/***************************************************************************
 * interface operations
 ***************************************************************************/

void      blame_map_init(); 
void      blame_map_add_blame(uint64_t obj, uint32_t metric_value);
uint64_t  blame_map_get_blame(uint64_t obj);

/******************************************************************************
 * data type
 *****************************************************************************/

struct blame_parts {
  uint32_t obj_id;
  uint32_t blame;
};


typedef union {
  uint64_t all;
  struct blame_parts parts; //[0] is id, [1] is blame
} blame_entry;

// global variable
extern volatile blame_entry table[];

#endif // _hpctoolkit_blame_map_h_