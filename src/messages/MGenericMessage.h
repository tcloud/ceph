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


#ifndef CEPH_MGENERICMESSAGE_H
#define CEPH_MGENERICMESSAGE_H

#include "msg/Message.h"

class MGenericMessage : public Message {
  char tname[20];
  //long pcid;

 public:
  MGenericMessage(int t) : Message(t) { 
    snprintf(tname, sizeof(tname), "generic%d", get_type());
  }

  //void set_pcid(long pcid) { this->pcid = pcid; }
  //long get_pcid() { return pcid; }

  const char *get_type_name() { return tname; }

  void decode_payload(CephContext *cct) { }
  void encode_payload(CephContext *cct) { }
};

#endif
