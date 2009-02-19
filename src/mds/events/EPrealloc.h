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

#ifndef __MDS_EPREALLOC_H
#define __MDS_EPREALLOC_H

#include "../LogEvent.h"
#include "EMetaBlob.h"

class EPrealloc : public LogEvent {
public:
  int client;
  inodeno_t start, len;
  version_t tablev;

  EPrealloc() : LogEvent(EVENT_PREALLOC) { }
  EPrealloc(MDLog *mdlog, 
	    int c, inodeno_t s, inodeno_t l, version_t v) : 
    LogEvent(EVENT_PREALLOC),
    client(c), start(s), len(l), tablev(v) {}
  
  void print(ostream& out) {
    out << "EPrealloc client" << client << " " << start << "~" << len << " v" << tablev;
  }

  void encode(bufferlist &bl) const {
    ::encode(client, bl);
    ::encode(start, bl);
    ::encode(len, bl);
    ::encode(tablev, bl);
  } 
  void decode(bufferlist::iterator &bl) {
    ::decode(client, bl);
    ::decode(start, bl);
    ::decode(len, bl);
    ::decode(tablev, bl);
  }

  void update_segment();
  void replay(MDS *mds);
};

#endif
