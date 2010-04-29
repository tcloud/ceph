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


#ifndef __SIMPLELOCK_H
#define __SIMPLELOCK_H

// -- lock types --
// see CEPH_LOCK_*

inline const char *get_lock_type_name(int t) {
  switch (t) {
  case CEPH_LOCK_DN: return "dn";
  case CEPH_LOCK_IVERSION: return "iversion";
  case CEPH_LOCK_IFILE: return "ifile";
  case CEPH_LOCK_IAUTH: return "iauth";
  case CEPH_LOCK_ILINK: return "ilink";
  case CEPH_LOCK_IDFT: return "idft";
  case CEPH_LOCK_INEST: return "inest";
  case CEPH_LOCK_IXATTR: return "ixattr";
  case CEPH_LOCK_ISNAP: return "isnap";
  case CEPH_LOCK_INO: return "ino";
  case CEPH_LOCK_IFLOCK: return "iflock";
  default: assert(0); return 0;
  }
}

class Mutation;

extern "C" {
#include "locks.h"
}


#define CAP_ANY     0
#define CAP_LONER   1
#define CAP_XLOCKER 2

struct LockType {
  int type;
  sm_t *sm;

  LockType(int t) : type(t) {
    switch (type) {
    case CEPH_LOCK_DN:
    case CEPH_LOCK_IAUTH:
    case CEPH_LOCK_ILINK:
    case CEPH_LOCK_IXATTR:
    case CEPH_LOCK_ISNAP:
    case CEPH_LOCK_IFLOCK:
      sm = &sm_simplelock;
      break;
    case CEPH_LOCK_IDFT:
    case CEPH_LOCK_INEST:
      sm = &sm_scatterlock;
      break;
    case CEPH_LOCK_IFILE:
      sm = &sm_filelock;
      break;
    case CEPH_LOCK_IVERSION:
      sm = &sm_locallock;
      break;
    default:
      sm = 0;
    }
  }

};


class SimpleLock {
public:
  LockType *type;
  
  virtual const char *get_state_name(int n) {
    switch (n) {
    case LOCK_UNDEF: return "UNDEF";
    case LOCK_SYNC: return "sync";
    case LOCK_LOCK: return "lock";

    case LOCK_PREXLOCK: return "prexlock";
    case LOCK_XLOCK: return "xlock";
    case LOCK_XLOCKDONE: return "xlockdone";
    case LOCK_LOCK_XLOCK: return "lock->prexlock";

    case LOCK_SYNC_LOCK: return "sync->lock";
    case LOCK_LOCK_SYNC: return "lock->sync";
    case LOCK_REMOTEXLOCK: return "remote_xlock";
    case LOCK_EXCL: return "excl";
    case LOCK_EXCL_SYNC: return "excl->sync";
    case LOCK_EXCL_LOCK: return "excl->lock";
    case LOCK_SYNC_EXCL: return "sync->excl";
    case LOCK_LOCK_EXCL: return "lock->excl";      

    case LOCK_SYNC_MIX: return "sync->mix";
    case LOCK_SYNC_MIX2: return "sync->mix(2)";
    case LOCK_LOCK_TSYN: return "lock->tsyn";
      
    case LOCK_MIX_LOCK: return "mix->lock";
    case LOCK_MIX: return "mix";
    case LOCK_MIX_TSYN: return "mix->tsyn";
      
    case LOCK_TSYN_MIX: return "tsyn->mix";
    case LOCK_TSYN_LOCK: return "tsyn->lock";
    case LOCK_TSYN: return "tsyn";

    case LOCK_MIX_SYNC: return "mix->sync";
    case LOCK_MIX_SYNC2: return "mix->sync(2)";
    case LOCK_EXCL_MIX: return "excl->mix";
    case LOCK_MIX_EXCL: return "mix->excl";

    case LOCK_PRE_SCAN: return "*->scan";
    case LOCK_SCAN: return "scan";

    default: assert(0); return 0;
    }
  }


  // waiting
  static const __u64 WAIT_RD          = (1<<0);  // to read
  static const __u64 WAIT_WR          = (1<<1);  // to write
  static const __u64 WAIT_XLOCK       = (1<<2);  // to xlock   (** dup)
  static const __u64 WAIT_STABLE      = (1<<2);  // for a stable state
  static const __u64 WAIT_REMOTEXLOCK = (1<<3);  // for a remote xlock
  static const int WAIT_BITS        = 4;
  static const __u64 WAIT_ALL         = ((1<<WAIT_BITS)-1);


protected:
  // parent (what i lock)
  MDSCacheObject *parent;

  // lock state
  __s16 state;

private:
  __s16 num_rdlock;
  __s32 num_client_lease;

  struct unstable_bits_t {
    set<__s32> gather_set;  // auth+rep.  >= 0 is mds, < 0 is client

    // local state
    int num_wrlock, num_xlock;
    Mutation *xlock_by;
    client_t xlock_by_client;
    client_t excl_client;

    bool empty() {
      return
	gather_set.empty() &&
	num_wrlock == 0 &&
	num_xlock == 0 &&
	xlock_by == NULL &&
	xlock_by_client == -1 &&
	excl_client == -1;
    }

    unstable_bits_t() : num_wrlock(0),
			num_xlock(0),
			xlock_by(NULL),
			xlock_by_client(-1),
			excl_client(-1) {}
  };

  mutable unstable_bits_t *_unstable;

  bool have_more() const { return _unstable ? true : false; }
  unstable_bits_t *more() const {
    if (!_unstable)
      _unstable = new unstable_bits_t;
    return _unstable;
  }
  void clear_more() {
    if (_unstable) {
      assert(_unstable->empty());
      delete _unstable;
      _unstable = NULL;
    }
  }
  void try_clear_more() {
    if (_unstable && _unstable->empty()) {
      delete _unstable;
      _unstable = NULL;
    }
  }

public:

  void set_excl_client(client_t c) { more()->excl_client = c; }
  client_t get_excl_client() { return have_more() ? more()->excl_client : -1; }



  SimpleLock(MDSCacheObject *o, LockType *lt) :
    type(lt),
    parent(o), 
    state(LOCK_SYNC),
    num_rdlock(0),
    num_client_lease(0),
    _unstable(NULL)
  {}
  virtual ~SimpleLock() {
    delete _unstable;
  }

  // parent
  MDSCacheObject *get_parent() { return parent; }
  int get_type() { return type->type; }
  const sm_t* get_sm() const { return type->sm; }

  int get_wait_shift() {
    switch (get_type()) {
    case CEPH_LOCK_DN:       return 8;
    case CEPH_LOCK_IAUTH:    return 5 +   SimpleLock::WAIT_BITS;
    case CEPH_LOCK_ILINK:    return 5 +   SimpleLock::WAIT_BITS;
    case CEPH_LOCK_IDFT:     return 5 + 2*SimpleLock::WAIT_BITS;
    case CEPH_LOCK_IFILE:    return 5 + 3*SimpleLock::WAIT_BITS;
    case CEPH_LOCK_IVERSION: return 5 + 4*SimpleLock::WAIT_BITS;
    case CEPH_LOCK_IXATTR:   return 5 + 5*SimpleLock::WAIT_BITS;
    case CEPH_LOCK_ISNAP:    return 5 + 6*SimpleLock::WAIT_BITS;
    case CEPH_LOCK_INEST:    return 5 + 7*SimpleLock::WAIT_BITS;
    case CEPH_LOCK_IFLOCK:    return 5 + 8*SimpleLock::WAIT_BITS;
    default:
      assert(0);
    }
  }

  int get_cap_shift() {
    switch (get_type()) {
    case CEPH_LOCK_IAUTH: return CEPH_CAP_SAUTH;
    case CEPH_LOCK_ILINK: return CEPH_CAP_SLINK;
    case CEPH_LOCK_IFILE: return CEPH_CAP_SFILE;
    case CEPH_LOCK_IXATTR: return CEPH_CAP_SXATTR;
    default: return 0;
    }
  }
  int get_cap_mask() {
    switch (get_type()) {
    case CEPH_LOCK_IFILE: return 0xffff;
    default: return 0x3;
    }
  }

  struct ptr_lt {
    bool operator()(const SimpleLock* l, const SimpleLock* r) const {
      // first sort by object type (dn < inode)
      if ((l->type->type>CEPH_LOCK_DN) <  (r->type->type>CEPH_LOCK_DN)) return true;
      if ((l->type->type>CEPH_LOCK_DN) == (r->type->type>CEPH_LOCK_DN)) {
	// then sort by object
	if (l->parent->is_lt(r->parent)) return true;
	if (l->parent == r->parent) {
	  // then sort by (inode) lock type
	  if (l->type->type < r->type->type) return true;
	}
      }
      return false;
    }
  };

  void decode_locked_state(bufferlist& bl) {
    parent->decode_lock_state(type->type, bl);
  }
  void encode_locked_state(bufferlist& bl) {
    parent->encode_lock_state(type->type, bl);
  }
  void finish_waiters(__u64 mask, int r=0) {
    parent->finish_waiting(mask << get_wait_shift(), r);
  }
  void take_waiting(__u64 mask, list<Context*>& ls) {
    parent->take_waiting(mask << get_wait_shift(), ls);
  }
  void add_waiter(__u64 mask, Context *c) {
    parent->add_waiter(mask << get_wait_shift(), c);
  }
  bool is_waiter_for(__u64 mask) {
    return parent->is_waiter_for(mask << get_wait_shift());
  }
  
  

  // state
  int get_state() { return state; }
  int set_state(int s) { 
    state = s; 
    //assert(!is_stable() || gather_set.size() == 0);  // gather should be empty in stable states.
    return s;
  }
  void set_state_rejoin(int s, list<Context*>& waiters) {
    if (!is_stable()) {
      state = s;
      get_parent()->auth_unpin(this);
    } else {
      state = s;
    }
    take_waiting(SimpleLock::WAIT_ALL, waiters);
  }

  bool is_stable() {
    return get_sm()->states[state].next == 0;
  }
  int get_next_state() {
    return get_sm()->states[state].next;
  }

  bool fw_rdlock_to_auth() {
    return get_sm()->states[state].can_rdlock == FW;
  }
  bool req_rdlock_from_auth() {
    return get_sm()->states[state].can_rdlock == REQ;
  }

  // gather set
  static set<int> empty_gather_set;
  const set<int>& get_gather_set() { return have_more() ? more()->gather_set : empty_gather_set; }
  void init_gather() {
    for (map<int,int>::const_iterator p = parent->replicas_begin(); 
	 p != parent->replicas_end(); 
	 ++p)
      more()->gather_set.insert(p->first);
  }
  bool is_gathering() { return have_more() && !more()->gather_set.empty(); }
  bool is_gathering(int i) {
    return have_more() && more()->gather_set.count(i);
  }
  void clear_gather() {
    if (have_more())
      more()->gather_set.clear();
  }
  void remove_gather(int i) {
    if (have_more())
      more()->gather_set.erase(i);
  }



  virtual bool is_updated() { return false; }


  // can_*
  bool can_lease(client_t client) {
    return get_sm()->states[state].can_lease == ANY ||
      (get_sm()->states[state].can_lease == AUTH && parent->is_auth()) ||
      (get_sm()->states[state].can_lease == XCL && client >= 0 && get_xlock_by_client() == client);
  }
  bool can_read(client_t client) {
    return get_sm()->states[state].can_read == ANY ||
      (get_sm()->states[state].can_read == AUTH && parent->is_auth()) ||
      (get_sm()->states[state].can_read == XCL && client >= 0 && get_xlock_by_client() == client);
  }
  bool can_read_projected(client_t client) {
    return get_sm()->states[state].can_read_projected == ANY ||
      (get_sm()->states[state].can_read_projected == AUTH && parent->is_auth()) ||
      (get_sm()->states[state].can_read_projected == XCL && client >= 0 && get_xlock_by_client() == client);
  }
  bool can_rdlock(client_t client) {
    return get_sm()->states[state].can_rdlock == ANY ||
      (get_sm()->states[state].can_rdlock == AUTH && parent->is_auth()) ||
      (get_sm()->states[state].can_rdlock == XCL && client >= 0 && get_xlock_by_client() == client);
  }
  bool can_wrlock(client_t client) {
    return get_sm()->states[state].can_wrlock == ANY ||
      (get_sm()->states[state].can_wrlock == AUTH && parent->is_auth()) ||
      (get_sm()->states[state].can_wrlock == XCL && client >= 0 && (get_xlock_by_client() == client ||
								    get_excl_client() == client));
  }
  bool can_xlock(client_t client) {
    return get_sm()->states[state].can_xlock == ANY ||
      (get_sm()->states[state].can_xlock == AUTH && parent->is_auth()) ||
      (get_sm()->states[state].can_xlock == XCL && client >= 0 && get_xlock_by_client() == client);
  }

  // rdlock
  bool is_rdlocked() { return num_rdlock > 0; }
  int get_rdlock() { 
    if (!num_rdlock)
      parent->get(MDSCacheObject::PIN_LOCK);
    return ++num_rdlock; 
  }
  int put_rdlock() {
    assert(num_rdlock>0);
    --num_rdlock;
    if (num_rdlock == 0)
      parent->put(MDSCacheObject::PIN_LOCK);
    return num_rdlock;
  }
  int get_num_rdlocks() { return num_rdlock; }

  // wrlock
  void get_wrlock(bool force=false) {
    //assert(can_wrlock() || force);
    if (more()->num_wrlock == 0)
      parent->get(MDSCacheObject::PIN_LOCK);
    ++more()->num_wrlock;
  }
  void put_wrlock() {
    --more()->num_wrlock;
    if (more()->num_wrlock == 0) {
      parent->put(MDSCacheObject::PIN_LOCK);
      try_clear_more();
    }
  }
  bool is_wrlocked() { return have_more() && more()->num_wrlock > 0; }
  int get_num_wrlocks() { return have_more() ? more()->num_wrlock : 0; }

  // xlock
  void get_xlock(Mutation *who, client_t client) { 
    assert(get_xlock_by() == 0);
    assert(state == LOCK_XLOCK);
    parent->get(MDSCacheObject::PIN_LOCK);
    more()->num_xlock++;
    more()->xlock_by = who; 
    more()->xlock_by_client = client;
  }
  void set_xlock_done() {
    assert(more()->xlock_by);
    assert(state == LOCK_XLOCK);
    state = LOCK_XLOCKDONE;
    more()->xlock_by = 0;
  }
  void put_xlock() {
    assert(state == LOCK_XLOCK || state == LOCK_XLOCKDONE);
    --more()->num_xlock;
    parent->put(MDSCacheObject::PIN_LOCK);
    if (more()->num_xlock == 0) {
      more()->xlock_by = 0;
      more()->xlock_by_client = -1;
      try_clear_more();
    }
  }
  bool is_xlocked() { return have_more() && more()->num_xlock > 0; }
  int get_num_xlocks() { return have_more() ? more()->num_xlock : 0; }
  client_t get_xlock_by_client() {
    return have_more() ? more()->xlock_by_client : -1;
  }
  bool is_xlocked_by_client(client_t c) {
    return have_more() ? more()->xlock_by_client == c : false;
  }
  Mutation *get_xlock_by() { return have_more() ? more()->xlock_by : NULL; }
  
  // lease
  void get_client_lease() {
    num_client_lease++;
  }
  void put_client_lease() {
    assert(num_client_lease > 0);
    num_client_lease--;
    if (num_client_lease == 0) {
      try_clear_more();
    }
  }
  bool is_leased() { return num_client_lease > 0; }
  int get_num_client_lease() {
    return num_client_lease;
  }

  bool is_used() {
    return is_xlocked() || is_rdlocked() || is_wrlocked() || num_client_lease;
  }

  // encode/decode
  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(state, bl);
    if (have_more())
      ::encode(more()->gather_set, bl);
    else
      ::encode(empty_gather_set, bl);
  }
  void decode(bufferlist::iterator& p) {
    __u8 struct_v;
    ::decode(struct_v, p);
    ::decode(state, p);
    set<int> g;
    ::decode(g, p);
    if (!g.empty())
      more()->gather_set.swap(g);
  }
  void encode_state_for_replica(bufferlist& bl) const {
    __s16 s = get_replica_state();
    ::encode(s, bl);
  }
  void decode_state(bufferlist::iterator& p, bool is_new=true) {
    __s16 s;
    ::decode(s, p);
    if (is_new)
      state = s;
  }
  void decode_state_rejoin(bufferlist::iterator& p, list<Context*>& waiters) {
    __s16 s;
    ::decode(s, p);
    set_state_rejoin(s, waiters);
  }


  // caps
  bool is_loner_mode() {
    return get_sm()->states[state].loner;
  }
  int gcaps_allowed_ever() {
    return parent->is_auth() ? get_sm()->allowed_ever_auth : get_sm()->allowed_ever_replica;
  }
  int gcaps_allowed(int who, int s=-1) {
    if (s < 0) s = state;
    if (parent->is_auth()) {
      if (get_xlock_by_client() >= 0 && who == CAP_XLOCKER)
	return get_sm()->states[s].xlocker_caps;
      else if (is_loner_mode() && who == CAP_ANY)
	return get_sm()->states[s].caps;
      else 
	return get_sm()->states[s].loner_caps | get_sm()->states[s].caps;  // loner always gets more
    } else 
      return get_sm()->states[s].replica_caps;
  }
  int gcaps_careful() {
    if (get_num_wrlocks())
      return get_sm()->careful;
    return 0;
  }


  int gcaps_xlocker_mask(client_t client) {
    if (client == get_xlock_by_client())
      return type->type == CEPH_LOCK_IFILE ? 0xffff : (CEPH_CAP_GSHARED|CEPH_CAP_GEXCL);
    return 0;
  }

  // simplelock specifics
  int get_replica_state() const {
    return get_sm()->states[state].replica_state;
  }
  void export_twiddle() {
    clear_gather();
    state = get_replica_state();
  }

  /** replicate_relax
   * called on first replica creation.
   */
  void replicate_relax() {
    assert(parent->is_auth());
    assert(!parent->is_replicated());
    if (state == LOCK_LOCK && !is_used())
      state = LOCK_SYNC;
  }
  bool remove_replica(int from) {
    if (is_gathering(from)) {
      remove_gather(from);
      if (!is_gathering())
	return true;
    }
    return false;
  }
  bool do_import(int from, int to) {
    if (!is_stable()) {
      remove_gather(from);
      remove_gather(to);
      if (!is_gathering())
	return true;
    }
    if (!is_stable() && !is_gathering())
      return true;
    return false;
  }

  void _print(ostream& out) {
    out << get_lock_type_name(get_type()) << " ";
    out << get_state_name(get_state());
    if (!get_gather_set().empty())
      out << " g=" << get_gather_set();
    if (num_client_lease)
      out << " l=" << num_client_lease;
    if (is_rdlocked()) 
      out << " r=" << get_num_rdlocks();
    if (is_wrlocked()) 
      out << " w=" << get_num_wrlocks();
    if (is_xlocked()) {
      out << " x=" << get_num_xlocks();
      if (get_xlock_by())
	out << " by " << get_xlock_by();
    }
    /*if (is_stable())
      out << " stable";
    else
      out << " unstable";
    */
  }

  virtual void print(ostream& out) {
    out << "(";
    _print(out);
    out << ")";
  }
};
WRITE_CLASS_ENCODER(SimpleLock)

inline ostream& operator<<(ostream& out, SimpleLock& l) 
{
  l.print(out);
  return out;
}


#endif
