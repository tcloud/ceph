// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "MasterInoTable.h"
#include "MDS.h"

#include "include/types.h"

#include "config.h"

#define DOUT_SUBSYS mds
#undef dout_prefix
#define dout_prefix *_dout << dbeginl << "mds" << mds->get_nodeid() << "." << table_name << ": "

void MasterInoTable::init_inode()
{
  ino = MDS_INO_MASTERINOTABLE;
  layout = g_default_file_layout;
}

void MasterInoTable::reset_state()
{
  free.clear();
  prealloc.clear();

  uint64_t start = (uint64_t)(MAX_MDS+1) << 40;
  uint64_t end = ((uint64_t)(-1));
  free.insert(start, end-start);

  projected_free.m = free.m;
}

void MasterInoTable::project_prealloc(int client, inodeno_t& start, unsigned& len)
{
  assert(is_active());

  dout(10) << "project_prealloc client" << client << " " << len << " from " << projected_free << dendl;

  start = projected_free.start();
  inodeno_t end = projected_free.end_after(start);
  if (end - start < len)
    len = end-start;

  dout(10) << "project_prealloc client" << client << " " << start << "~" << len
	   << " to " << projected_free << "/" << free << dendl;

  projected_free.erase(start, len);
  projected_prealloc[client].insert(start, len);
  ++projected_version;
}

void MasterInoTable::apply_prealloc(int client, inodeno_t start, unsigned len)
{
  dout(10) << "apply_prealloc client" << client << " " << start << "~" << len << dendl;
  free.erase(start, len);
  prealloc[client].insert(start, len);
  ++version;
}

void MasterInoTable::replay_prealloc(int client, inodeno_t start, unsigned len) 
{
  dout(10) << "replay_prealloc client" << client << " " << start << "~" << len << dendl;
  free.erase(start, len);
  projected_free.erase(start, len);
  prealloc[client].insert(start, len);
  projected_prealloc[client].insert(start, len);
  projected_version = ++version;
}
