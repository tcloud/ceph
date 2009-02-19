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


#ifndef __MASTERINOTABLE_H
#define __MASTERINOTABLE_H

#include "InoTable.h"
#include "MDSTable.h"
#include "include/interval_set.h"

class MDS;

class MasterInoTable : public MDSTable {
  interval_set<inodeno_t> free, projected_free;
  map<int, interval_set<inodeno_t> > prealloc, projected_prealloc;

 public:
  MasterInoTable(MDS *m) : MDSTable(m, "masterinotable") { }

  void project_prealloc(int client, inodeno_t& start, unsigned& len);
  void apply_prealloc(int client, inodeno_t start, unsigned len);
  void replay_prealloc(int client, inodeno_t start, unsigned len);

  void project_reap(int client, inodeno_t start, inodeno_t len);

  void init_inode();
  void reset_state();
  void encode_state(bufferlist& bl) {
    ::encode(free, bl);
    ::encode(prealloc, bl);
  }
  void decode_state(bufferlist::iterator& bl) {
    ::decode(free, bl);
    ::decode(prealloc, bl);

    projected_free.m = free.m;
    projected_prealloc = prealloc;
  }
};

#endif
