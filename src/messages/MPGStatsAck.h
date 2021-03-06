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

#ifndef CEPH_MPGSTATSACK_H
#define CEPH_MPGSTATSACK_H

#include "osd/osd_types.h"

class MPGStatsAck : public Message {
public:
  map<pg_t,eversion_t> pg_stat;
  
  MPGStatsAck() : Message(MSG_PGSTATSACK) {}

private:
  ~MPGStatsAck() {}

public:
  const char *get_type_name() { return "pg_stats_ack"; }
  void print(ostream& out) {
    out << "pg_stats_ack(" << pg_stat.size() << " pgs)";
  }

  void encode_payload(CephContext *cct) {
    ::encode(pg_stat, payload);
  }
  void decode_payload(CephContext *cct) {
    bufferlist::iterator p = payload.begin();
    ::decode(pg_stat, p);
  }
};

#endif
