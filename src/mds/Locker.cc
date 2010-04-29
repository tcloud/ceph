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


#include "MDS.h"
#include "MDCache.h"
#include "Locker.h"
#include "CInode.h"
#include "CDir.h"
#include "CDentry.h"

#include "MDLog.h"
#include "MDSMap.h"

#include "include/filepath.h"

#include "events/EString.h"
#include "events/EUpdate.h"
#include "events/EOpen.h"

#include "msg/Messenger.h"

#include "messages/MGenericMessage.h"
#include "messages/MDiscover.h"
#include "messages/MDiscoverReply.h"

#include "messages/MDirUpdate.h"

#include "messages/MInodeFileCaps.h"

#include "messages/MLock.h"
#include "messages/MClientLease.h"
#include "messages/MDentryUnlink.h"

#include "messages/MClientRequest.h"
#include "messages/MClientReply.h"
#include "messages/MClientCaps.h"
#include "messages/MClientCapRelease.h"

#include "messages/MMDSSlaveRequest.h"

#include <errno.h>

#include "config.h"


#define DOUT_SUBSYS mds
#undef dout_prefix
#define dout_prefix _prefix(mds)
static ostream& _prefix(MDS *mds) {
  return *_dout << dbeginl << "mds" << mds->get_nodeid() << ".locker ";
}


void Locker::dispatch(Message *m)
{

  switch (m->get_type()) {

    // inter-mds locking
  case MSG_MDS_LOCK:
    handle_lock((MLock*)m);
    break;
    // inter-mds caps
  case MSG_MDS_INODEFILECAPS:
    handle_inode_file_caps((MInodeFileCaps*)m);
    break;

    // client sync
  case CEPH_MSG_CLIENT_CAPS:
    handle_client_caps((MClientCaps*)m);
    break;
  case CEPH_MSG_CLIENT_CAPRELEASE:
    handle_client_cap_release((MClientCapRelease*)m);
    break;
  case CEPH_MSG_CLIENT_LEASE:
    handle_client_lease((MClientLease*)m);
    break;
    
  default:
    assert(0);
  }
}


/*
 * locks vs rejoin
 *
 * 
 *
 */

void Locker::send_lock_message(SimpleLock *lock, int msg)
{
  for (map<int,int>::iterator it = lock->get_parent()->replicas_begin(); 
       it != lock->get_parent()->replicas_end(); 
       it++) {
    if (mds->mdsmap->get_state(it->first) < MDSMap::STATE_REJOIN) 
      continue;
    MLock *m = new MLock(lock, msg, mds->get_nodeid());
    mds->send_message_mds(m, it->first);
  }
}

void Locker::send_lock_message(SimpleLock *lock, int msg, const bufferlist &data)
{
  for (map<int,int>::iterator it = lock->get_parent()->replicas_begin(); 
       it != lock->get_parent()->replicas_end(); 
       it++) {
    if (mds->mdsmap->get_state(it->first) < MDSMap::STATE_REJOIN) 
      continue;
    MLock *m = new MLock(lock, msg, mds->get_nodeid());
    m->set_data(data);
    mds->send_message_mds(m, it->first);
  }
}




void Locker::include_snap_rdlocks(set<SimpleLock*>& rdlocks, CInode *in)
{
  // rdlock ancestor snaps
  CInode *t = in;
  rdlocks.insert(&in->snaplock);
  while (t->get_projected_parent_dn()) {
    t = t->get_projected_parent_dn()->get_dir()->get_inode();
    rdlocks.insert(&t->snaplock);
  }
}



bool Locker::acquire_locks(MDRequest *mdr,
			   set<SimpleLock*> &rdlocks,
			   set<SimpleLock*> &wrlocks,
			   set<SimpleLock*> &xlocks)
{
  if (mdr->done_locking) {
    dout(10) << "acquire_locks " << *mdr << " -- done locking" << dendl;    
    return true;  // at least we had better be!
  }
  dout(10) << "acquire_locks " << *mdr << dendl;

  client_t client = mdr->get_client();

  set<SimpleLock*, SimpleLock::ptr_lt> sorted;  // sort everything we will lock
  set<SimpleLock*> mustpin = xlocks;            // items to authpin

  // xlocks
  for (set<SimpleLock*>::iterator p = xlocks.begin(); p != xlocks.end(); ++p) {
    dout(20) << " must xlock " << **p << " " << *(*p)->get_parent() << dendl;
    sorted.insert(*p);

    // augment xlock with a versionlock?
    if ((*p)->get_type() > CEPH_LOCK_IVERSION) {
      // inode version lock?
      CInode *in = (CInode*)(*p)->get_parent();
      if (mdr->is_master()) {
	// master.  wrlock versionlock so we can pipeline inode updates to journal.
	wrlocks.insert(&in->versionlock);
      } else {
	// slave.  exclusively lock the inode version (i.e. block other journal updates).
	// this makes rollback safe.
	xlocks.insert(&in->versionlock);
	sorted.insert(&in->versionlock);
      }
    }
  }

  // wrlocks
  for (set<SimpleLock*>::iterator p = wrlocks.begin(); p != wrlocks.end(); ++p) {
    dout(20) << " must wrlock " << **p << " " << *(*p)->get_parent() << dendl;
    sorted.insert(*p);
    if ((*p)->get_parent()->is_auth())
      mustpin.insert(*p);
    else if ((*p)->get_type() == CEPH_LOCK_IFILE &&
	     !(*p)->get_parent()->is_auth() && !(*p)->can_wrlock(client)) { // we might have to request a scatter
      dout(15) << " will also auth_pin " << *(*p)->get_parent() << " in case we need to request a scatter" << dendl;
      mustpin.insert(*p);
    }
  }

  // rdlocks
  for (set<SimpleLock*>::iterator p = rdlocks.begin();
	 p != rdlocks.end();
       ++p) {
    dout(20) << " must rdlock " << **p << " " << *(*p)->get_parent() << dendl;
    sorted.insert(*p);
    if ((*p)->get_parent()->is_auth())
      mustpin.insert(*p);
    else if ((*p)->get_type() == CEPH_LOCK_IFILE &&
	     !(*p)->get_parent()->is_auth() && !(*p)->can_rdlock(client)) { // we might have to request an rdlock
      dout(15) << " will also auth_pin " << *(*p)->get_parent() << " in case we need to request a rdlock" << dendl;
      mustpin.insert(*p);
    }
  }

 
  // AUTH PINS
  map<int, set<MDSCacheObject*> > mustpin_remote;  // mds -> (object set)
  
  // can i auth pin them all now?
  for (set<SimpleLock*>::iterator p = mustpin.begin();
       p != mustpin.end();
       ++p) {
    MDSCacheObject *object = (*p)->get_parent();

    dout(10) << " must authpin " << *object << dendl;

    if (mdr->is_auth_pinned(object)) 
      continue;
    
    if (!object->is_auth()) {
      if (object->is_ambiguous_auth()) {
	// wait
	dout(10) << " ambiguous auth, waiting to authpin " << *object << dendl;
	object->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, new C_MDS_RetryRequest(mdcache, mdr));
	mds->locker->drop_locks(mdr);
	mdr->drop_local_auth_pins();
	return false;
      }
      mustpin_remote[object->authority().first].insert(object);
      continue;
    }
    if (!object->can_auth_pin()) {
      // wait
      dout(10) << " can't auth_pin (freezing?), waiting to authpin " << *object << dendl;
      object->add_waiter(MDSCacheObject::WAIT_UNFREEZE, new C_MDS_RetryRequest(mdcache, mdr));
      mds->locker->drop_locks(mdr);
      mdr->drop_local_auth_pins();
      return false;
    }
  }

  // ok, grab local auth pins
  for (set<SimpleLock*>::iterator p = mustpin.begin();
       p != mustpin.end();
       ++p) {
    MDSCacheObject *object = (*p)->get_parent();
    if (mdr->is_auth_pinned(object)) {
      dout(10) << " already auth_pinned " << *object << dendl;
    } else if (object->is_auth()) {
      dout(10) << " auth_pinning " << *object << dendl;
      mdr->auth_pin(object);
    }
  }

  // request remote auth_pins
  if (!mustpin_remote.empty()) {
    for (map<int, set<MDSCacheObject*> >::iterator p = mustpin_remote.begin();
	 p != mustpin_remote.end();
	 ++p) {
      dout(10) << "requesting remote auth_pins from mds" << p->first << dendl;
      
      MMDSSlaveRequest *req = new MMDSSlaveRequest(mdr->reqid, MMDSSlaveRequest::OP_AUTHPIN);
      for (set<MDSCacheObject*>::iterator q = p->second.begin();
	   q != p->second.end();
	   ++q) {
	dout(10) << " req remote auth_pin of " << **q << dendl;
	MDSCacheObjectInfo info;
	(*q)->set_object_info(info);
	req->get_authpins().push_back(info);      
	mdr->pin(*q);
      }
      mds->send_message_mds(req, p->first);

      // put in waiting list
      assert(mdr->more()->waiting_on_slave.count(p->first) == 0);
      mdr->more()->waiting_on_slave.insert(p->first);
    }
    return false;
  }

  // acquire locks.
  // make sure they match currently acquired locks.
  set<SimpleLock*, SimpleLock::ptr_lt>::iterator existing = mdr->locks.begin();
  for (set<SimpleLock*, SimpleLock::ptr_lt>::iterator p = sorted.begin();
       p != sorted.end();
       ++p) {

    // already locked?
    if (existing != mdr->locks.end() && *existing == *p) {
      // right kind?
      SimpleLock *have = *existing;
      existing++;
      if (xlocks.count(*p) && mdr->xlocks.count(*p)) {
	dout(10) << " already xlocked " << *have << " " << *have->get_parent() << dendl;
      }
      else if (wrlocks.count(*p) && mdr->wrlocks.count(*p)) {
	dout(10) << " already wrlocked " << *have << " " << *have->get_parent() << dendl;
      }
      else if (rdlocks.count(*p) && mdr->rdlocks.count(*p)) {
	dout(10) << " already rdlocked " << *have << " " << *have->get_parent() << dendl;
      }
      else assert(0);
      continue;
    }
    
    // hose any stray locks
    while (existing != mdr->locks.end()) {
      SimpleLock *stray = *existing;
      existing++;
      dout(10) << " unlocking out-of-order " << *stray << " " << *stray->get_parent() << dendl;
      if (mdr->xlocks.count(stray)) 
	xlock_finish(stray, mdr);
      else if (mdr->wrlocks.count(stray))
	wrlock_finish(stray, mdr);
      else
	rdlock_finish(stray, mdr);
    }
      
    // lock
    if (xlocks.count(*p)) {
      if (!xlock_start(*p, mdr)) 
	return false;
      dout(10) << " got xlock on " << **p << " " << *(*p)->get_parent() << dendl;
    } else if (wrlocks.count(*p)) {
      if (!wrlock_start(*p, mdr)) 
	return false;
      dout(10) << " got wrlock on " << **p << " " << *(*p)->get_parent() << dendl;
    } else {
      if (!rdlock_start(*p, mdr)) 
	return false;
      dout(10) << " got rdlock on " << **p << " " << *(*p)->get_parent() << dendl;
    }
  }
    
  // any extra unneeded locks?
  while (existing != mdr->locks.end()) {
    SimpleLock *stray = *existing;
    existing++;
    dout(10) << " unlocking extra " << *stray << " " << *stray->get_parent() << dendl;
    if (mdr->xlocks.count(stray))
      xlock_finish(stray, mdr);
    else if (mdr->wrlocks.count(stray))
      wrlock_finish(stray, mdr);
    else
      rdlock_finish(stray, mdr);
  }

  mdr->done_locking = true;
  return true;
}


void Locker::set_xlocks_done(Mutation *mut)
{
  for (set<SimpleLock*>::iterator p = mut->xlocks.begin();
       p != mut->xlocks.end();
       p++) {
    dout(10) << "set_xlocks_done on " << **p << " " << *(*p)->get_parent() << dendl;
    (*p)->set_xlock_done();
  }
}

void Locker::drop_locks(Mutation *mut)
{
  // leftover locks
  while (!mut->xlocks.empty()) 
    xlock_finish(*mut->xlocks.begin(), mut);
  while (!mut->rdlocks.empty()) 
    rdlock_finish(*mut->rdlocks.begin(), mut);
  while (!mut->wrlocks.empty()) 
    wrlock_finish(*mut->wrlocks.begin(), mut);
}

void Locker::drop_rdlocks(Mutation *mut)
{
  while (!mut->rdlocks.empty()) 
    rdlock_finish(*mut->rdlocks.begin(), mut);
}


// generics

void Locker::eval_gather(SimpleLock *lock, bool first, bool *pneed_issue)
{
  dout(10) << "eval_gather " << *lock << " on " << *lock->get_parent() << dendl;
  assert(!lock->is_stable());

  int next = lock->get_next_state();

  CInode *in = 0;
  bool caps = lock->get_cap_shift();
  if (lock->get_type() != CEPH_LOCK_DN)
    in = (CInode *)lock->get_parent();

  bool need_issue = false;

  int loner_issued = 0, other_issued = 0, xlocker_issued = 0;
  if (caps) {
    in->get_caps_issued(&loner_issued, &other_issued, &xlocker_issued, lock->get_cap_shift(), 3);
    dout(10) << " next state is " << lock->get_state_name(next) 
	     << " issued/allows loner " << gcap_string(loner_issued)
	     << "/" << gcap_string(lock->gcaps_allowed(CAP_LONER, next))
	     << " xlocker " << gcap_string(xlocker_issued)
	     << "/" << gcap_string(lock->gcaps_allowed(CAP_XLOCKER, next))
	     << " other " << gcap_string(other_issued)
	     << "/" << gcap_string(lock->gcaps_allowed(CAP_ANY, next))
	     << dendl;

    if (first && ((~lock->gcaps_allowed(CAP_ANY, next) & other_issued) ||
		  (~lock->gcaps_allowed(CAP_LONER, next) & loner_issued) ||
		  (~lock->gcaps_allowed(CAP_XLOCKER, next) & xlocker_issued)))
      need_issue = true;
  }

  if (!lock->is_gathering() &&
      (lock->get_sm()->states[next].can_rdlock || !lock->is_rdlocked()) &&
      (lock->get_sm()->states[next].can_wrlock || !lock->is_wrlocked()) &&
      (lock->get_sm()->states[next].can_xlock || !lock->is_xlocked()) &&
      (lock->get_sm()->states[next].can_lease || !lock->is_leased()) &&
      (!caps || ((~lock->gcaps_allowed(CAP_ANY, next) & other_issued) == 0 &&
		 (~lock->gcaps_allowed(CAP_LONER, next) & loner_issued) == 0 &&
		 (~lock->gcaps_allowed(CAP_XLOCKER, next) & xlocker_issued) == 0)) &&
      lock->get_state() != LOCK_SYNC_MIX2 &&  // these states need an explicit trigger from the auth mds
      lock->get_state() != LOCK_MIX_SYNC2
      ) {
    dout(7) << "eval_gather finished gather on " << *lock
	    << " on " << *lock->get_parent() << dendl;

    if (lock->get_sm() == &sm_filelock) {
      if (in->state_test(CInode::STATE_NEEDSRECOVER)) {
	dout(7) << "eval_gather finished gather, but need to recover" << dendl;
	mds->mdcache->queue_file_recover(in);
	mds->mdcache->do_file_recover();
      }
      if (in->state_test(CInode::STATE_RECOVERING)) {
	dout(7) << "eval_gather finished gather, but still recovering" << dendl;
	return;
      }
    }

    if (!lock->get_parent()->is_auth()) {
      // replica: tell auth
      int auth = lock->get_parent()->authority().first;

      if (mds->mdsmap->get_state(auth) >= MDSMap::STATE_REJOIN) {
	switch (lock->get_state()) {
	case LOCK_SYNC_LOCK:
	  mds->send_message_mds(new MLock(lock, LOCK_AC_LOCKACK, mds->get_nodeid()),
				auth);
	  break;

	case LOCK_MIX_SYNC:
	  {
	    MLock *reply = new MLock(lock, LOCK_AC_SYNCACK, mds->get_nodeid());
	    lock->encode_locked_state(reply->get_data());
	    mds->send_message_mds(reply, auth);
	    next = LOCK_MIX_SYNC2;
	    ((ScatterLock *)lock)->start_flush();
	  }
	  break;

	case LOCK_MIX_SYNC2:
	  ((ScatterLock *)lock)->finish_flush();

	case LOCK_SYNC_MIX2:
	  // do nothing, we already acked
	  break;
	  
	case LOCK_SYNC_MIX:
	  { 
	    MLock *reply = new MLock(lock, LOCK_AC_MIXACK, mds->get_nodeid());
	    mds->send_message_mds(reply, auth);
	    next = LOCK_SYNC_MIX2;
	  }
	  break;

	case LOCK_MIX_LOCK:
	  {
	    bufferlist data;
	    lock->encode_locked_state(data);
	    mds->send_message_mds(new MLock(lock, LOCK_AC_LOCKACK, mds->get_nodeid(), data), auth);
	    ((ScatterLock *)lock)->start_flush();
	  }
	  break;

	default:
	  assert(0);
	}
      }
    } else {
      // auth

      if (lock->is_updated()) {
	scatter_writebehind((ScatterLock*)lock);
	mds->mdlog->flush();
	return;
      }
      
      switch (lock->get_state()) {
	// to mixed
      case LOCK_TSYN_MIX:
      case LOCK_SYNC_MIX:
      case LOCK_EXCL_MIX:
	if (lock->get_parent()->is_replicated()) {
	  bufferlist softdata;
	  lock->encode_locked_state(softdata);
	  send_lock_message(lock, LOCK_AC_MIX, softdata);
	}
	((ScatterLock*)lock)->clear_scatter_wanted();
	break;

	// to sync
      case LOCK_EXCL_SYNC:
      case LOCK_LOCK_SYNC:
      case LOCK_MIX_SYNC:
      case LOCK_XLOCKDONE:
	if (lock->get_parent()->is_replicated()) {
	  bufferlist softdata;
	  lock->encode_locked_state(softdata);
	  send_lock_message(lock, LOCK_AC_SYNC, softdata);
	}
	break;
      }

    }

    lock->set_state(next);
    
    if (lock->get_parent()->is_auth() &&
	lock->is_stable())
      lock->get_parent()->auth_unpin(lock);

    // drop loner before doing waiters
    if (caps &&
	in->is_auth() && in->get_loner() >= 0 && in->get_wanted_loner() < 0) {
      dout(10) << "  trying to drop loner" << dendl;
      if (in->try_drop_loner()) {
	dout(10) << "  dropped loner" << dendl;
	need_issue = true;
      }
    }

    lock->finish_waiters(SimpleLock::WAIT_STABLE|SimpleLock::WAIT_WR|SimpleLock::WAIT_RD|SimpleLock::WAIT_XLOCK);
    
    if (caps)
      need_issue = true;

    if (lock->get_parent()->is_auth() &&
	lock->is_stable())
      eval(lock, &need_issue);
  }

  if (need_issue) {
    if (pneed_issue)
      *pneed_issue = true;
    else
      issue_caps(in);
  }

}

bool Locker::eval(CInode *in, int mask)
{
  bool need_issue = false;
  
  dout(10) << "eval " << mask << " " << *in << dendl;

  // choose loner?
  if (in->is_auth()) {
    if (in->choose_ideal_loner() >= 0) {
      if (in->try_set_loner()) {
	dout(10) << "eval set loner to client" << in->get_loner() << dendl;
	need_issue = true;
	mask = -1;
      } else
	dout(10) << "eval want loner client" << in->get_wanted_loner() << " but failed to set it" << dendl;
    } else
      dout(10) << "eval doesn't want loner" << dendl;
  }
    
  if (mask & CEPH_LOCK_IFILE)
    eval_any(&in->filelock, &need_issue);
  if (mask & CEPH_LOCK_IAUTH)
    eval_any(&in->authlock, &need_issue);
  if (mask & CEPH_LOCK_ILINK)
    eval_any(&in->linklock, &need_issue);
  if (mask & CEPH_LOCK_IXATTR)
    eval_any(&in->xattrlock, &need_issue);
  if (mask & CEPH_LOCK_INEST)
    eval_any(&in->nestlock, &need_issue);
  if (mask & CEPH_LOCK_IFLOCK)
    eval_any(&in->flocklock, &need_issue);

  // drop loner?
  if (in->is_auth() && in->get_loner() >= 0 && in->get_wanted_loner() < 0) {
    dout(10) << "  trying to drop loner" << dendl;
    if (in->try_drop_loner()) {
      dout(10) << "  dropped loner" << dendl;
      need_issue = true;
    }
  }

  if (need_issue)
    issue_caps(in);

  dout(10) << "eval done" << dendl;
  return need_issue;
}

class C_Locker_Eval : public Context {
  Locker *locker;
  CInode *in;
  int mask;
public:
  C_Locker_Eval(Locker *l, CInode *i, int m) : locker(l), in(i), mask(m) {
    in->get(CInode::PIN_PTRWAITER);    
  }
  void finish(int r) {
    in->put(CInode::PIN_PTRWAITER);
    locker->try_eval(in, mask);
  }
};

void Locker::try_eval(CInode *in, int mask)
{
  // unstable and ambiguous auth?
  if (in->is_ambiguous_auth()) {
    dout(7) << "try_eval not ambiguous auth, waiting on " << *in << dendl;
    in->add_waiter(CInode::WAIT_SINGLEAUTH, new C_Locker_Eval(this, in, mask));
    return;
  }

  if (!in->is_auth()) {
    dout(7) << "try_eval not auth for " << *in << dendl;
    return;
  }

  if (!in->can_auth_pin()) {
    dout(7) << "try_eval can't auth_pin, waiting on " << *in << dendl;
    in->add_waiter(CInode::WAIT_UNFREEZE, new C_Locker_Eval(this, in, mask));
    return;
  }

  eval(in, mask);
}

void Locker::eval_cap_gather(CInode *in)
{
  bool need_issue = false;

  // kick locks now
  if (!in->filelock.is_stable())
    eval_gather(&in->filelock, false, &need_issue);
  if (!in->authlock.is_stable())
    eval_gather(&in->authlock, false, &need_issue);
  if (!in->linklock.is_stable())
    eval_gather(&in->linklock, false, &need_issue);
  if (!in->xattrlock.is_stable())
    eval_gather(&in->xattrlock, false, &need_issue);

  if (need_issue)
    issue_caps(in);
}

void Locker::eval(SimpleLock *lock, bool *need_issue)
{
  switch (lock->get_type()) {
  case CEPH_LOCK_IFILE:
    return file_eval((ScatterLock*)lock, need_issue);
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_INEST:
    return scatter_eval((ScatterLock*)lock, need_issue);
  default:
    return simple_eval(lock, need_issue);
  }
}


// ------------------
// rdlock

bool Locker::_rdlock_kick(SimpleLock *lock)
{
  // kick the lock
  if (lock->is_stable()) {
    if (lock->get_parent()->is_auth()) {
      if (lock->get_sm() == &sm_scatterlock) {
	if (lock->get_parent()->is_replicated())
	  scatter_tempsync((ScatterLock*)lock);
	else
	  simple_sync(lock);
      } else 
	simple_sync(lock);
      return true;
    } else {
      // request rdlock state change from auth
      int auth = lock->get_parent()->authority().first;
      dout(10) << "requesting rdlock from auth on " 
	       << *lock << " on " << *lock->get_parent() << dendl;
      mds->send_message_mds(new MLock(lock, LOCK_AC_REQRDLOCK, mds->get_nodeid()), auth);
      return false;
    }
  }
  return false;
}

bool Locker::rdlock_try(SimpleLock *lock, client_t client, Context *con)
{
  dout(7) << "rdlock_try on " << *lock << " on " << *lock->get_parent() << dendl;  

  // can read?  grab ref.
  if (lock->can_rdlock(client)) 
    return true;
  
  _rdlock_kick(lock);

  if (lock->can_rdlock(client)) 
    return true;

  // wait!
  dout(7) << "rdlock_try waiting on " << *lock << " on " << *lock->get_parent() << dendl;
  if (con)
    lock->add_waiter(SimpleLock::WAIT_STABLE|SimpleLock::WAIT_RD, con);
  return false;
}

bool Locker::rdlock_start(SimpleLock *lock, MDRequest *mut)
{
  dout(7) << "rdlock_start  on " << *lock << " on " << *lock->get_parent() << dendl;  

  // client may be allowed to rdlock the same item it has xlocked.
  client_t client = mut->get_client();

  if (!lock->get_parent()->is_auth() &&
      lock->fw_rdlock_to_auth()) {
    mdcache->request_forward(mut, lock->get_parent()->authority().first);
    return false;
  }

  while (1) {
    // can read?  grab ref.
    if (lock->can_rdlock(client)) {
      lock->get_rdlock();
      mut->rdlocks.insert(lock);
      mut->locks.insert(lock);
      return true;
    }

    if (!_rdlock_kick(lock))
      break;
  }

  // wait!
  int wait_on;
  if (lock->get_parent()->is_auth() && lock->is_stable())
    wait_on = SimpleLock::WAIT_RD;
  else
    wait_on = SimpleLock::WAIT_STABLE;  // REQRDLOCK is ignored if lock is unstable, so we need to retry.
  dout(7) << "rdlock_start waiting on " << *lock << " on " << *lock->get_parent() << dendl;
  lock->add_waiter(wait_on, new C_MDS_RetryRequest(mdcache, mut));
  nudge_log(lock);
  return false;
}

void Locker::nudge_log(SimpleLock *lock)
{
  dout(10) << "nudge_log " << *lock << " on " << *lock->get_parent() << dendl;
  if (lock->get_parent()->is_auth() && !lock->is_stable())    // as with xlockdone, or cap flush
    mds->mdlog->flush();
}

void Locker::rdlock_finish(SimpleLock *lock, Mutation *mut)
{
  // drop ref
  lock->put_rdlock();
  if (mut) {
    mut->rdlocks.erase(lock);
    mut->locks.erase(lock);
  }

  dout(7) << "rdlock_finish on " << *lock << " on " << *lock->get_parent() << dendl;
  
  // last one?
  if (!lock->is_rdlocked()) {
    if (!lock->is_stable())
      eval_gather(lock);
    else if (lock->get_parent()->is_auth())
      eval(lock);
  }
}



// ------------------
// wrlock

void Locker::wrlock_force(SimpleLock *lock, Mutation *mut)
{
  if (lock->get_type() == CEPH_LOCK_IVERSION)
    return local_wrlock_grab((LocalLock*)lock, mut);

  dout(7) << "wrlock_force  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  lock->get_wrlock(true);
  mut->wrlocks.insert(lock);
  mut->locks.insert(lock);
}

bool Locker::wrlock_start(SimpleLock *lock, MDRequest *mut, bool nowait)
{
  if (lock->get_type() == CEPH_LOCK_IVERSION)
    return local_wrlock_start((LocalLock*)lock, mut);

  dout(10) << "wrlock_start " << *lock << " on " << *lock->get_parent() << dendl;

  bool want_scatter = lock->get_parent()->is_auth() &&
    ((CInode*)lock->get_parent())->has_subtree_root_dirfrag();
    
  CInode *in = (CInode *)lock->get_parent();
  client_t client = mut->get_client();
  
  while (1) {
    // wrlock?
    if (lock->can_wrlock(client)) {
      lock->get_wrlock();
      mut->wrlocks.insert(lock);
      mut->locks.insert(lock);
      return true;
    }

    if (!lock->is_stable())
      break;

    if (in->is_auth()) {
      if (want_scatter)
	file_mixed((ScatterLock*)lock);
      else {
	if (nowait && lock->is_updated())
	  return false;   // don't do nested lock, as that may scatter_writebehind in simple_lock!
	simple_lock(lock);
      }

      if (nowait && !lock->can_wrlock(client))
	return false;
      
    } else {
      // replica.
      // auth should be auth_pinned (see acquire_locks wrlock weird mustpin case).
      int auth = lock->get_parent()->authority().first;
      dout(10) << "requesting scatter from auth on " 
	       << *lock << " on " << *lock->get_parent() << dendl;
      mds->send_message_mds(new MLock(lock, LOCK_AC_REQSCATTER, mds->get_nodeid()), auth);
      break;
    }
  }

  if (!nowait) {
    dout(7) << "wrlock_start waiting on " << *lock << " on " << *lock->get_parent() << dendl;
    lock->add_waiter(SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
    nudge_log(lock);
  }
    
  return false;
}

void Locker::wrlock_finish(SimpleLock *lock, Mutation *mut)
{
  if (lock->get_type() == CEPH_LOCK_IVERSION)
    return local_wrlock_finish((LocalLock*)lock, mut);

  dout(7) << "wrlock_finish on " << *lock << " on " << *lock->get_parent() << dendl;
  lock->put_wrlock();
  mut->wrlocks.erase(lock);
  mut->locks.erase(lock);

  if (!lock->is_wrlocked()) {
    if (!lock->is_stable())
      eval_gather(lock);
    else if (lock->get_parent()->is_auth())
      eval(lock);
  }
}




// ------------------
// xlock

bool Locker::xlock_start(SimpleLock *lock, MDRequest *mut)
{
  if (lock->get_type() == CEPH_LOCK_IVERSION)
    return local_xlock_start((LocalLock*)lock, mut);

  dout(7) << "xlock_start on " << *lock << " on " << *lock->get_parent() << dendl;
  client_t client = mut->get_client();

  // auth?
  if (lock->get_parent()->is_auth()) {
    // auth
    while (1) {
      if (lock->can_xlock(client)) {
	lock->set_state(LOCK_XLOCK);
	lock->get_xlock(mut, client);
	mut->xlocks.insert(lock);
	mut->locks.insert(lock);
	return true;
      }
      
      if (!lock->is_stable() && lock->get_state() != LOCK_XLOCKDONE)
	break;

      if (lock->get_state() == LOCK_LOCK || lock->get_state() == LOCK_XLOCKDONE)
	simple_xlock(lock);
      else
	simple_lock(lock);
    }
    
    lock->add_waiter(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
    nudge_log(lock);
    return false;
  } else {
    // replica
    assert(lock->get_sm()->can_remote_xlock);
    assert(!mut->slave_request);
    
    // wait for single auth
    if (lock->get_parent()->is_ambiguous_auth()) {
      lock->get_parent()->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, 
				     new C_MDS_RetryRequest(mdcache, mut));
      return false;
    }
    
    // send lock request
    int auth = lock->get_parent()->authority().first;
    mut->more()->slaves.insert(auth);
    MMDSSlaveRequest *r = new MMDSSlaveRequest(mut->reqid, MMDSSlaveRequest::OP_XLOCK);
    r->set_lock_type(lock->get_type());
    lock->get_parent()->set_object_info(r->get_object_info());
    mds->send_message_mds(r, auth);
    
    // wait
    lock->add_waiter(SimpleLock::WAIT_REMOTEXLOCK, new C_MDS_RetryRequest(mdcache, mut));
    return false;
  }
}

void Locker::xlock_finish(SimpleLock *lock, Mutation *mut)
{
  if (lock->get_type() == CEPH_LOCK_IVERSION)
    return local_xlock_finish((LocalLock*)lock, mut);

  dout(10) << "xlock_finish on " << *lock << " " << *lock->get_parent() << dendl;

  // drop ref
  lock->put_xlock();
  assert(mut);
  mut->xlocks.erase(lock);
  mut->locks.erase(lock);
  
  bool do_issue = true;

  // remote xlock?
  if (!lock->get_parent()->is_auth()) {
    assert(lock->get_sm()->can_remote_xlock);

    // tell auth
    dout(7) << "xlock_finish releasing remote xlock on " << *lock->get_parent()  << dendl;
    int auth = lock->get_parent()->authority().first;
    if (mds->mdsmap->get_state(auth) >= MDSMap::STATE_REJOIN) {
      MMDSSlaveRequest *slavereq = new MMDSSlaveRequest(mut->reqid, MMDSSlaveRequest::OP_UNXLOCK);
      slavereq->set_lock_type(lock->get_type());
      lock->get_parent()->set_object_info(slavereq->get_object_info());
      mds->send_message_mds(slavereq, auth);
    }
    // others waiting?
    lock->finish_waiters(SimpleLock::WAIT_STABLE |
			 SimpleLock::WAIT_WR | 
			 SimpleLock::WAIT_RD, 0); 
  } else {
    if (lock->get_num_xlocks() == 0 &&
	lock->get_num_rdlocks() == 0 &&
	lock->get_num_wrlocks() == 0 &&
	lock->get_num_client_lease() == 0) {
      assert(!lock->is_stable());
      lock->get_parent()->auth_unpin(lock);
      if (lock->get_type() != CEPH_LOCK_DN && ((CInode*)lock->get_parent())->get_loner() >= 0)
	lock->set_state(LOCK_EXCL);
      else
	lock->set_state(LOCK_LOCK);
      do_issue = true;
    }

    // others waiting?
    lock->finish_waiters(SimpleLock::WAIT_STABLE |
			 SimpleLock::WAIT_WR | 
			 SimpleLock::WAIT_RD, 0); 
  }
    
  // eval?
  if (!lock->is_stable())
    eval_gather(lock);
  else {
    if (lock->get_parent()->is_auth())
      eval(lock);
    if (do_issue && lock->get_type() != CEPH_LOCK_DN)
      issue_caps((CInode*)lock->get_parent());
  }
}



// file i/o -----------------------------------------

version_t Locker::issue_file_data_version(CInode *in)
{
  dout(7) << "issue_file_data_version on " << *in << dendl;
  return in->inode.file_data_version;
}

struct C_Locker_FileUpdate_finish : public Context {
  Locker *locker;
  CInode *in;
  Mutation *mut;
  bool share;
  client_t client;
  Capability *cap;
  MClientCaps *ack;
  C_Locker_FileUpdate_finish(Locker *l, CInode *i, Mutation *m, bool e=false, client_t c=-1,
			     Capability *cp = 0,
			     MClientCaps *ac = 0) : 
    locker(l), in(i), mut(m), share(e), client(c), cap(cp),
    ack(ac) {
    in->get(CInode::PIN_PTRWAITER);
  }
  void finish(int r) {
    locker->file_update_finish(in, mut, share, client, cap, ack);
  }
};

void Locker::file_update_finish(CInode *in, Mutation *mut, bool share, client_t client, 
				Capability *cap, MClientCaps *ack)
{
  dout(10) << "file_update_finish on " << *in << dendl;
  in->pop_and_dirty_projected_inode(mut->ls);
  in->put(CInode::PIN_PTRWAITER);

  mut->apply();
  
  if (ack)
    mds->send_message_client_counted(ack, client);

  drop_locks(mut);
  mut->cleanup();
  delete mut;

  bool sup = false;  // avoid sending two caps msgs, one for cap expansion, one for file_max change.
  if (cap && (cap->wanted() & ~cap->pending())) {
    issue_caps(in, cap);
    cap->inc_suppress();
    sup = true;
  }
  if (sup)
    cap->dec_suppress();
  
  if (share && in->is_auth() && in->filelock.is_stable())
    share_inode_max_size(in);

  // unlinked stray?  may need to purge (e.g., after all caps are released)
  mdcache->maybe_eval_stray(in);
}

Capability* Locker::issue_new_caps(CInode *in,
				   int mode,
				   Session *session,
				   SnapRealm *realm,
				   bool is_replay)
{
  dout(7) << "issue_new_caps for mode " << mode << " on " << *in << dendl;
  bool is_new;

  // if replay, try to reconnect cap, and otherwise do nothing.
  if (is_replay) {
    mds->mdcache->try_reconnect_cap(in, session);
    return 0;
  }

  // my needs
  assert(session->inst.name.is_client());
  int my_client = session->inst.name.num();
  int my_want = ceph_caps_for_mode(mode);

  // register a capability
  Capability *cap = in->get_client_cap(my_client);
  if (!cap) {
    // new cap
    cap = in->add_client_cap(my_client, session, realm);
    cap->set_wanted(my_want);
    cap->inc_suppress(); // suppress file cap messages for new cap (we'll bundle with the open() reply)
    is_new = true;
  } else {
    is_new = false;
    // make sure it wants sufficient caps
    if (my_want & ~cap->wanted()) {
      // augment wanted caps for this client
      cap->set_wanted(cap->wanted() | my_want);
    }
  }

  if (in->is_auth()) {
    // [auth] twiddle mode?
    eval(in, CEPH_CAP_LOCKS);

    if (!in->filelock.is_stable() ||
	!in->authlock.is_stable() ||
	!in->linklock.is_stable() ||
	!in->xattrlock.is_stable())
      mds->mdlog->flush();

  } else {
    // [replica] tell auth about any new caps wanted
    request_inode_file_caps(in);
  }

  // issue caps (pot. incl new one)
  //issue_caps(in);  // note: _eval above may have done this already...

  // re-issue whatever we can
  //cap->issue(cap->pending());

  if (is_new)
    cap->dec_suppress();

  return cap;
}



bool Locker::issue_caps(CInode *in, Capability *only_cap)
{
  // allowed caps are determined by the lock mode.
  int all_allowed = in->get_caps_allowed_by_type(CAP_ANY);
  int loner_allowed = in->get_caps_allowed_by_type(CAP_LONER);
  int xlocker_allowed = in->get_caps_allowed_by_type(CAP_XLOCKER);

  client_t loner = in->get_loner();
  if (loner >= 0) {
    dout(7) << "issue_caps loner client" << loner
	    << " allowed=" << ccap_string(loner_allowed) 
	    << ", xlocker allowed=" << ccap_string(xlocker_allowed)
	    << ", others allowed=" << ccap_string(all_allowed)
	    << " on " << *in << dendl;
  } else {
    dout(7) << "issue_caps allowed=" << ccap_string(all_allowed) 
	    << ", xlocker allowed=" << ccap_string(xlocker_allowed)
	    << " on " << *in << dendl;
  }

  // count conflicts with
  int nissued = 0;        

  // client caps
  map<client_t, Capability*>::iterator it;
  if (only_cap)
    it = in->client_caps.find(only_cap->get_client());
  else
    it = in->client_caps.begin();
  for (; it != in->client_caps.end(); it++) {
    Capability *cap = it->second;
    if (cap->is_stale())
      continue;

    // do not issue _new_ bits when size|mtime is projected
    int allowed;
    if (loner == it->first)
      allowed = loner_allowed;
    else
      allowed = all_allowed;

    // add in any xlocker-only caps (for locks this client is the xlocker for)
    allowed |= xlocker_allowed & in->get_xlocker_mask(it->first);

    int pending = cap->pending();
    int wanted = cap->wanted();

    dout(20) << " client" << it->first
	     << " pending " << ccap_string(pending) 
	     << " allowed " << ccap_string(allowed) 
	     << " wanted " << ccap_string(wanted)
	     << dendl;

    // skip if suppress, and not revocation
    if (cap->is_suppress() && !(pending & ~allowed)) {
      dout(20) << "  suppressed and !revoke, skipping client" << it->first << dendl;
      continue;
    }

    // are there caps that the client _wants_ and can have, but aren't pending?
    // or do we need to revoke?
    if (((wanted & allowed) & ~pending) ||  // missing wanted+allowed caps
	(pending & ~allowed)) {             // need to revoke ~allowed caps.
      // issue
      nissued++;

      // include caps that clients generally like, while we're at it.
      int likes = in->get_caps_liked();      
      int before = pending;
      long seq;
      if (pending & ~allowed)
	seq = cap->issue((wanted|likes) & allowed & pending);  // if revoking, don't issue anything new.
      else
	seq = cap->issue((wanted|likes) & allowed);
      int after = cap->pending();

      if (seq > 0) {
        dout(7) << "   sending MClientCaps to client" << it->first
		<< " seq " << cap->get_last_seq()
		<< " new pending " << ccap_string(after) << " was " << ccap_string(before) 
		<< dendl;

	MClientCaps *m = new MClientCaps((before & ~after) ? CEPH_CAP_OP_REVOKE:CEPH_CAP_OP_GRANT,
					 in->ino(),
					 in->find_snaprealm()->inode->ino(),
					 cap->get_cap_id(), cap->get_last_seq(),
					 after, wanted, 0,
					 cap->get_mseq());
	in->encode_cap_message(m, cap);

	mds->send_message_client_counted(m, it->first);
      }
    }

    if (only_cap)
      break;
  }

  return (nissued == 0);  // true if no re-issued, no callbacks
}

void Locker::issue_truncate(CInode *in)
{
  dout(7) << "issue_truncate on " << *in << dendl;
  
  for (map<client_t, Capability*>::iterator it = in->client_caps.begin();
       it != in->client_caps.end();
       it++) {
    Capability *cap = it->second;
    MClientCaps *m = new MClientCaps(CEPH_CAP_OP_TRUNC,
				     in->ino(),
				     in->find_snaprealm()->inode->ino(),
				     cap->get_cap_id(), cap->get_last_seq(),
				     cap->pending(), cap->wanted(), 0,
				     cap->get_mseq());
    in->encode_cap_message(m, cap);			     
    mds->send_message_client_counted(m, it->first);
  }

  // should we increase max_size?
  if (in->is_auth() && in->is_file())
    check_inode_max_size(in);
}

void Locker::revoke_stale_caps(Session *session)
{
  dout(10) << "revoke_stale_caps for " << session->inst.name << dendl;
  client_t client = session->get_client();

  for (xlist<Capability*>::iterator p = session->caps.begin(); !p.end(); ++p) {
    Capability *cap = *p;
    cap->set_stale(true);
    CInode *in = cap->get_inode();
    int issued = cap->issued();
    if (issued) {
      dout(10) << " revoking " << ccap_string(issued) << " on " << *in << dendl;      
      cap->revoke();

      if (in->is_auth() &&
	  in->inode.client_ranges.count(client))
	in->state_set(CInode::STATE_NEEDSRECOVER);

      if (!in->filelock.is_stable()) eval_gather(&in->filelock);
      if (!in->authlock.is_stable()) eval_gather(&in->authlock);
      if (!in->xattrlock.is_stable()) eval_gather(&in->xattrlock);

      if (in->is_auth()) {
	if (in->filelock.is_stable()) eval(&in->filelock);
	if (in->authlock.is_stable()) eval(&in->authlock);
	if (in->xattrlock.is_stable()) eval(&in->xattrlock);
      } else {
	request_inode_file_caps(in);
      }
    } else {
      dout(10) << " nothing issued on " << *in << dendl;
    }
  }
}

void Locker::resume_stale_caps(Session *session)
{
  dout(10) << "resume_stale_caps for " << session->inst.name << dendl;

  for (xlist<Capability*>::iterator p = session->caps.begin(); !p.end(); ++p) {
    Capability *cap = *p;
    CInode *in = cap->get_inode();
    if (cap->is_stale()) {
      dout(10) << " clearing stale flag on " << *in << dendl;
      cap->set_stale(false);
      if (!in->is_auth() || !eval(in, CEPH_CAP_LOCKS))
	issue_caps(in, cap);
    }
  }
}

void Locker::remove_stale_leases(Session *session)
{
  dout(10) << "remove_stale_leases for " << session->inst.name << dendl;
  for (xlist<ClientLease*>::iterator p = session->leases.begin(); !p.end(); ++p) {
    ClientLease *l = *p;
    CDentry *parent = (CDentry*)l->parent;
    dout(15) << " removing lease for " << l->mask << " on " << *parent << dendl;
    parent->remove_client_lease(l, l->mask, this);
  }
}


class C_MDL_RequestInodeFileCaps : public Context {
  Locker *locker;
  CInode *in;
public:
  C_MDL_RequestInodeFileCaps(Locker *l, CInode *i) : locker(l), in(i) {
    in->get(CInode::PIN_PTRWAITER);
  }
  void finish(int r) {
    in->put(CInode::PIN_PTRWAITER);
    if (!in->is_auth())
      locker->request_inode_file_caps(in);
  }
};

void Locker::request_inode_file_caps(CInode *in)
{
  assert(!in->is_auth());

  int wanted = in->get_caps_wanted();
  if (wanted != in->replica_caps_wanted) {

    if (wanted == 0) {
      if (in->replica_caps_wanted_keep_until > g_clock.recent_now()) {
        // ok, release them finally!
        in->replica_caps_wanted_keep_until.sec_ref() = 0;
        dout(7) << "request_inode_file_caps " << ccap_string(wanted)
                 << " was " << ccap_string(in->replica_caps_wanted) 
                 << " no keeping anymore " 
                 << " on " << *in 
                 << dendl;
      }
      else if (in->replica_caps_wanted_keep_until.sec() == 0) {
        in->replica_caps_wanted_keep_until = g_clock.recent_now();
        in->replica_caps_wanted_keep_until.sec_ref() += 2;
        
        dout(7) << "request_inode_file_caps " << ccap_string(wanted)
                 << " was " << ccap_string(in->replica_caps_wanted) 
                 << " keeping until " << in->replica_caps_wanted_keep_until
                 << " on " << *in 
                 << dendl;
        return;
      } else {
        // wait longer
        return;
      }
    } else {
      in->replica_caps_wanted_keep_until.sec_ref() = 0;
    }
    assert(!in->is_auth());

    // wait for single auth
    if (in->is_ambiguous_auth()) {
      in->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, 
		     new C_MDL_RequestInodeFileCaps(this, in));
      return;
    }

    int auth = in->authority().first;
    dout(7) << "request_inode_file_caps " << ccap_string(wanted)
            << " was " << ccap_string(in->replica_caps_wanted) 
            << " on " << *in << " to mds" << auth << dendl;
    assert(!in->is_auth());

    in->replica_caps_wanted = wanted;

    if (mds->mdsmap->get_state(auth) >= MDSMap::STATE_REJOIN)
      mds->send_message_mds(new MInodeFileCaps(in->ino(), mds->get_nodeid(),
					       in->replica_caps_wanted),
			    auth);
  } else {
    in->replica_caps_wanted_keep_until.sec_ref() = 0;
  }
}

void Locker::handle_inode_file_caps(MInodeFileCaps *m)
{
  // nobody should be talking to us during recovery.
  assert(mds->is_rejoin() || mds->is_clientreplay() || mds->is_active() || mds->is_stopping());

  // ok
  CInode *in = mdcache->get_inode(m->get_ino());
  assert(in);
  assert(in->is_auth());

  if (mds->is_rejoin() &&
      in->is_rejoining()) {
    dout(7) << "handle_inode_file_caps still rejoining " << *in << ", dropping " << *m << dendl;
    m->put();
    return;
  }

  
  dout(7) << "handle_inode_file_caps replica mds" << m->get_from() << " wants caps " << ccap_string(m->get_caps()) << " on " << *in << dendl;

  if (m->get_caps())
    in->mds_caps_wanted[m->get_from()] = m->get_caps();
  else
    in->mds_caps_wanted.erase(m->get_from());

  try_eval(in, CEPH_CAP_LOCKS);
  m->put();
}


class C_MDL_CheckMaxSize : public Context {
  Locker *locker;
  CInode *in;
public:
  C_MDL_CheckMaxSize(Locker *l, CInode *i) : locker(l), in(i) {
    in->get(CInode::PIN_PTRWAITER);
  }
  void finish(int r) {
    in->put(CInode::PIN_PTRWAITER);
    if (in->is_auth())
      locker->check_inode_max_size(in);
  }
};


void Locker::calc_new_client_ranges(CInode *in, __u64 size, map<client_t,byte_range_t>& new_ranges)
{
  inode_t *latest = in->get_projected_inode();

  // increase ranges as appropriate.
  // shrink to 0 if no WR|BUFFER caps issued.
  for (map<client_t,Capability*>::iterator p = in->client_caps.begin();
       p != in->client_caps.end();
       p++) {
    if ((p->second->issued() | p->second->wanted()) & (CEPH_CAP_FILE_WR|CEPH_CAP_FILE_BUFFER)) {
      new_ranges[p->first].first = 0;
      if (latest->client_ranges.count(p->first))
	new_ranges[p->first].last = MAX(ROUND_UP_TO((size+1)<<1, latest->get_layout_size_increment()),
					latest->client_ranges[p->first].last);
      else
	new_ranges[p->first].last = ROUND_UP_TO((size+1)<<1, latest->get_layout_size_increment());
    }
  }

}

bool Locker::check_inode_max_size(CInode *in, bool force_wrlock,
				  bool update_size, __u64 new_size, utime_t new_mtime)
{
  assert(in->is_auth());

  inode_t *latest = in->get_projected_inode();
  map<client_t,byte_range_t> new_ranges;
  __u64 size = latest->size;
  if (update_size)
    size = new_size;
  bool new_max = false;

  calc_new_client_ranges(in, size, new_ranges);

  if (latest->client_ranges != new_ranges)
    new_max = true;

  if (!update_size && !new_max)
    return false;

  dout(10) << "check_inode_max_size new_ranges " << new_ranges
	   << " update_size " << update_size
	   << " on " << *in << dendl;

  if (in->is_frozen()) {
    dout(10) << "check_inode_max_size frozen, waiting on " << *in << dendl;
    in->add_waiter(CInode::WAIT_UNFREEZE, new C_MDL_CheckMaxSize(this, in));
    return false;
  }
  if (!force_wrlock && !in->filelock.can_wrlock(in->get_loner())) {
    // lock?
    if (in->filelock.is_stable()) {
      if (in->get_target_loner() >= 0)
	file_excl(&in->filelock);
      else
	simple_lock(&in->filelock);
    }
    if (!in->filelock.can_wrlock(in->get_loner())) {
      // try again later
      in->filelock.add_waiter(SimpleLock::WAIT_STABLE, new C_MDL_CheckMaxSize(this, in));
      dout(10) << "check_inode_max_size can't wrlock, waiting on " << *in << dendl;
      return false;    
    }
  }

  Mutation *mut = new Mutation;
  mut->ls = mds->mdlog->get_current_segment();
    
  inode_t *pi = in->project_inode();
  pi->version = in->pre_dirty();

  if (new_max) {
    dout(10) << "check_inode_max_size client_ranges " << pi->client_ranges << " -> " << new_ranges << dendl;
    pi->client_ranges = new_ranges;
  }

  if (update_size) {
    dout(10) << "check_inode_max_size size " << pi->size << " -> " << new_size << dendl;
    pi->size = new_size;
    pi->rstat.rbytes = new_size;
    dout(10) << "check_inode_max_size mtime " << pi->mtime << " -> " << new_mtime << dendl;
    pi->mtime = new_mtime;
  }

  // use EOpen if the file is still open; otherwise, use EUpdate.
  // this is just an optimization to push open files forward into
  // newer log segments.
  LogEvent *le;
  EMetaBlob *metablob;
  if (in->is_any_caps_wanted()) {   
    EOpen *eo = new EOpen(mds->mdlog);
    eo->add_ino(in->ino());
    metablob = &eo->metablob;
    le = eo;
    mut->ls->open_files.push_back(&in->item_open_file);
  } else {
    EUpdate *eu = new EUpdate(mds->mdlog, "check_inode_max_size");
    metablob = &eu->metablob;
    le = eu;
  }
  mds->mdlog->start_entry(le);
  if (update_size) {  // FIXME if/when we do max_size nested accounting
    mdcache->predirty_journal_parents(mut, metablob, in, 0, PREDIRTY_PRIMARY);
    // no cow, here!
    CDentry *parent = in->get_projected_parent_dn();
    metablob->add_primary_dentry(parent, true, in);
  } else {
    metablob->add_dir_context(in->get_projected_parent_dn()->get_dir());
    mdcache->journal_dirty_inode(mut, metablob, in);
  }
  mds->mdlog->submit_entry(le, new C_Locker_FileUpdate_finish(this, in, mut, true));
  wrlock_force(&in->filelock, mut);  // wrlock for duration of journal
  mut->auth_pin(in);

  // make max_size _increase_ timely
  if (new_max)
    mds->mdlog->flush();

  return true;
}


void Locker::share_inode_max_size(CInode *in)
{
  /*
   * only share if currently issued a WR cap.  if client doesn't have it,
   * file_max doesn't matter, and the client will get it if/when they get
   * the cap later.
   */
  dout(10) << "share_inode_max_size on " << *in << dendl;
  for (map<client_t,Capability*>::iterator it = in->client_caps.begin();
       it != in->client_caps.end();
       it++) {
    const client_t client = it->first;
    Capability *cap = it->second;
    if (cap->is_suppress())
      continue;
    if (cap->pending() & (CEPH_CAP_FILE_WR|CEPH_CAP_FILE_BUFFER)) {
      dout(10) << "share_inode_max_size with client" << client << dendl;
      MClientCaps *m = new MClientCaps(CEPH_CAP_OP_GRANT,
				       in->ino(),
				       in->find_snaprealm()->inode->ino(),
				       cap->get_cap_id(), cap->get_last_seq(),
				       cap->pending(), cap->wanted(), 0,
				       cap->get_mseq());
      in->encode_cap_message(m, cap);
      mds->send_message_client_counted(m, client);
    }
  }
}

void Locker::adjust_cap_wanted(Capability *cap, int wanted, int issue_seq)
{
  if (ceph_seq_cmp(issue_seq, cap->get_last_issue()) == 0) {
    dout(10) << " wanted " << ccap_string(cap->wanted())
	     << " -> " << ccap_string(wanted) << dendl;
    cap->set_wanted(wanted);
  } else if (wanted & ~cap->wanted()) {
    dout(10) << " wanted " << ccap_string(cap->wanted())
	     << " -> " << ccap_string(wanted)
	     << " (added caps even though we had seq mismatch!)" << dendl;
    cap->set_wanted(wanted | cap->wanted());
  } else {
    dout(10) << " NOT changing wanted " << ccap_string(cap->wanted())
	     << " -> " << ccap_string(wanted)
	     << " (issue_seq " << issue_seq << " != last_issue "
	     << cap->get_last_issue() << ")" << dendl;
    return;
  }

  CInode *cur = cap->get_inode();
  if (cap->wanted() == 0) {
    if (cur->item_open_file.is_on_list() &&
	!cur->is_any_caps_wanted()) {
      dout(10) << " removing unwanted file from open file list " << *cur << dendl;
      cur->item_open_file.remove_myself();
    }
  } else {
    if (!cur->item_open_file.is_on_list()) {
      dout(10) << " adding to open file list " << *cur << dendl;
      LogSegment *ls = mds->mdlog->get_current_segment();
      EOpen *le = new EOpen(mds->mdlog);
      mds->mdlog->start_entry(le);
      le->add_clean_inode(cur);
      ls->open_files.push_back(&cur->item_open_file);
      mds->mdlog->submit_entry(le);
    }
  }

}


/*
 * note: we only get these from the client if
 * - we are calling back previously issued caps (fewer than the client previously had)
 * - or if the client releases (any of) its caps on its own
 */
void Locker::handle_client_caps(MClientCaps *m)
{
  client_t client = m->get_source().num();

  snapid_t follows = m->get_snap_follows();
  dout(7) << "handle_client_caps on " << m->get_ino()
	  << " follows " << follows 
	  << " op " << ceph_cap_op_name(m->get_op()) << dendl;

  if (!mds->is_clientreplay() && !mds->is_active() && !mds->is_stopping()) {
    mds->wait_for_replay(new C_MDS_RetryMessage(mds, m));
    return;
  }

  CInode *head_in = mdcache->get_inode(m->get_ino());
  if (!head_in) {
    dout(7) << "handle_client_caps on unknown ino " << m->get_ino() << ", dropping" << dendl;
    m->put();
    return;
  }

  CInode *in = 0;
  if (head_in) {
    in = mdcache->pick_inode_snap(head_in, follows);
    if (in != head_in)
      dout(10) << " head inode " << *head_in << dendl;
    dout(10) << "  cap inode " << *in << dendl;
  }

  Capability *cap = 0;
  if (in) 
    cap = in->get_client_cap(client);
  if (!cap) {
    dout(7) << "handle_client_caps no cap for client" << client << " on " << *in << dendl;
    m->put();
    return;
  }  
  assert(cap);

  // freezing|frozen?
  if ((in->is_freezing() && (in->filelock.is_stable() &&
			     in->authlock.is_stable() &&
			     in->xattrlock.is_stable() &&
			     in->linklock.is_stable())) ||  // continue if freezing and lock is unstable
      in->is_frozen()) {
    dout(7) << "handle_client_caps freezing|frozen on " << *in << dendl;
    in->add_waiter(CInode::WAIT_UNFREEZE, new C_MDS_RetryMessage(mds, m));
    return;
  }
  if (ceph_seq_cmp(m->get_mseq(), cap->get_mseq()) < 0) {
    dout(7) << "handle_client_caps mseq " << m->get_mseq() << " < " << cap->get_mseq()
	    << ", dropping" << dendl;
    m->put();
    return;
  }

  int op = m->get_op();
  bool can_issue = true;

  // flushsnap?
  if (op == CEPH_CAP_OP_FLUSHSNAP) {
    if (in->is_auth()) {
      dout(7) << " flushsnap follows " << follows
	      << " client" << client << " on " << *in << dendl;
      // this cap now follows a later snap (i.e. the one initiating this flush, or later)
      cap->client_follows = follows+1;
      cap->confirm_receipt(cap->get_last_sent(), 0);
   
      // we can prepare the ack now, since this FLUSHEDSNAP is independent of any
      // other cap ops.  (except possibly duplicate FLUSHSNAP requests, but worst
      // case we get a dup response, so whatever.)
      MClientCaps *ack = 0;
      if (m->get_dirty()) {
	ack = new MClientCaps(CEPH_CAP_OP_FLUSHSNAP_ACK, in->ino(), 0, 0, 0, 0, 0, m->get_dirty(), 0);
	ack->set_snap_follows(follows);
	ack->set_client_tid(m->get_client_tid());
      }
      if (!_do_cap_update(in, cap, m->get_dirty(), follows, m, ack)) {
	if (ack)
	  mds->send_message_client_counted(ack, m->get_connection());
	eval_cap_gather(in);
      }

      // remove cap _after_ do_cap_update() (which takes the Capability*)
      if (in->last <= follows) {
	dout(10) << "  flushsnap releasing cloned cap" << dendl;
	in->remove_client_cap(client);
      } else {
	dout(10) << "  flushsnap NOT releasing live cap" << dendl;
      }      
    } else
      dout(7) << " not auth, ignoring flushsnap on " << *in << dendl;
    goto out;
  }


  // for this and all subsequent versions of this inode,
  while (1) {
    if (cap->get_cap_id() != m->get_cap_id()) {
      dout(7) << " ignoring client capid " << m->get_cap_id() << " != my " << cap->get_cap_id() << dendl;
    } else {
      // filter wanted based on what we could ever give out (given auth/replica status)
      cap->confirm_receipt(m->get_seq(), m->get_caps());
      dout(10) << " follows " << follows
	       << " retains " << ccap_string(m->get_caps())
	       << " dirty " << ccap_string(m->get_caps())
	       << " on " << *in << dendl;
      
      MClientCaps *ack = 0;
      
      if (m->get_dirty() && in->is_auth()) {
	dout(7) << " flush client" << client << " dirty " << ccap_string(m->get_dirty()) 
		<< " seq " << m->get_seq() << " on " << *in << dendl;
	ack = new MClientCaps(CEPH_CAP_OP_FLUSH_ACK, in->ino(), 0, cap->get_cap_id(), m->get_seq(),
			      m->get_caps(), 0, m->get_dirty(), 0);
	ack->set_client_tid(m->get_client_tid());
      }
      int new_wanted = m->get_wanted() & head_in->get_caps_allowed_ever();
      if (new_wanted != cap->wanted()) {
        if (new_wanted & ~cap->wanted()) {
          // exapnding caps.  make sure we aren't waiting for a log flush
          if (!in->filelock.is_stable() ||
	      !in->authlock.is_stable() ||
	      !in->xattrlock.is_stable())
            mds->mdlog->flush();
        }

	adjust_cap_wanted(cap, new_wanted, m->get_issue_seq());
      }
      if (m->get_op() == CEPH_CAP_OP_DROP)
	can_issue = false;
      
      if (in->is_auth() &&
	  _do_cap_update(in, cap, m->get_dirty(), follows, m, ack)) {
	// updated, cap msg is delayed
	cap->inc_suppress();
	eval(in, CEPH_CAP_LOCKS);
	cap->dec_suppress();

	if (cap->wanted() & ~cap->pending())
	  mds->mdlog->flush();
      } else {
	// no update, ack now.
	if (ack)
	  mds->send_message_client_counted(ack, m->get_connection());
      
	bool did_issue = eval(in, CEPH_CAP_LOCKS);
	if (!did_issue && (cap->wanted() & ~cap->pending()))
	  issue_caps(in, cap);
      }
    }

    // done?
    if (in->last == CEPH_NOSNAP)
      break;
    
    // next!
    in = mdcache->pick_inode_snap(in, in->last);
    cap = in->get_client_cap(client);
    assert(cap);    
  }
  
 out:
  m->put();
}

void Locker::process_cap_update(MDRequest *mdr, client_t client,
				inodeno_t ino, __u64 cap_id, int caps, int wanted,
				int seq, int issue_seq, int mseq,
				const nstring& dname)
{
  CInode *in = mdcache->get_inode(ino);
  if (!in)
    return;
  Capability *cap = in->get_client_cap(client);
  if (cap) {
    dout(10) << "process_cap_update client" << client << " " << ccap_string(caps) << " on " << *in << dendl;
    
    if (ceph_seq_cmp(mseq, cap->get_mseq()) < 0) {
      dout(7) << " mseq " << mseq << " < " << cap->get_mseq() << ", dropping" << dendl;
      return;
    }
    
    cap->confirm_receipt(seq, caps);
    adjust_cap_wanted(cap, wanted, issue_seq);

    cap->inc_suppress();
    eval(in, CEPH_CAP_LOCKS);
    cap->dec_suppress();

    // take note; we may need to reissue on this cap later
    mdr->cap_releases[in->vino()] = cap->get_last_seq();
  }
  
  if (dname.length()) {
    frag_t fg = in->pick_dirfrag(dname);
    CDir *dir = in->get_dirfrag(fg);
    if (dir) {
      CDentry *dn = dir->lookup(dname);
      ClientLease *l = dn->get_client_lease(client);
      if (l) {
	dout(10) << " removing lease on " << *dn << dendl;
	dn->remove_client_lease(l, l->mask, this);
      }
    }
  }
}

void Locker::kick_cap_releases(MDRequest *mdr)
{
  client_t client = mdr->get_client();
  for (map<vinodeno_t,ceph_seq_t>::iterator p = mdr->cap_releases.begin();
       p != mdr->cap_releases.end();
       p++) {
    CInode *in = mdcache->get_inode(p->first);
    if (!in)
      continue;
    Capability *cap = in->get_client_cap(client);
    if (!cap)
      continue;
    if (cap->get_last_sent() == p->second) {
      dout(10) << "kick_cap_releases released at current seq " << p->second
	       << ", reissuing" << dendl;
      issue_caps(in, cap);
    }
  }
}


static __u64 calc_bounding(__u64 t)
{
  t |= t >> 1;
  t |= t >> 2;
  t |= t >> 4;
  t |= t >> 8;
  t |= t >> 16;
  t |= t >> 32;
  return t + 1;
}

/*
 * update inode based on cap flush|flushsnap|wanted.
 *  adjust max_size, if needed.
 * if we update, return true; otherwise, false (no updated needed).
 */
bool Locker::_do_cap_update(CInode *in, Capability *cap,
			    int dirty, snapid_t follows, MClientCaps *m,
			    MClientCaps *ack)
{
  dout(10) << "_do_cap_update dirty " << ccap_string(dirty)
	   << " issued " << ccap_string(cap->issued())
	   << " wanted " << ccap_string(cap->wanted())
	   << " on " << *in << dendl;
  assert(in->is_auth());
  client_t client = m->get_source().num();
  inode_t *latest = in->get_projected_inode();

  // increase or zero max_size?
  __u64 size = m->get_size();
  bool change_max = false;
  uint64_t old_max = latest->client_ranges.count(client) ? latest->client_ranges[client].last : 0;
  uint64_t new_max = old_max;
  
  if (in->is_file()) {
    if ((cap->issued() | cap->wanted()) & CEPH_CAP_ANY_FILE_WR) {
      if (m->get_max_size() > new_max) {
	dout(10) << "client requests file_max " << m->get_max_size()
		 << " > max " << old_max << dendl;
	change_max = true;
	new_max = ROUND_UP_TO((m->get_max_size()+1) << 1, latest->get_layout_size_increment());
      } else {
	//new_max = ROUND_UP_TO((size+1)<<1, latest->get_layout_size_increment());
	new_max = calc_bounding(size * 2);
	if (new_max > old_max)
	  change_max = true;
	else
	  new_max = old_max;
      }
    } else {
      if (old_max) {
	change_max = true;
	new_max = 0;
      }
    }

    if (change_max &&
	!in->filelock.can_wrlock(client)) {
      dout(10) << " i want to change file_max, but lock won't allow it (yet)" << dendl;
      if (in->filelock.is_stable()) {
	bool need_issue = false;
	cap->inc_suppress();
	if (in->get_loner() >= 0 || (in->get_wanted_loner() >= 0 && in->try_set_loner())) {
	  if (in->filelock.get_state() != LOCK_EXCL)
	    file_excl(&in->filelock, &need_issue);
	  need_issue = false;  // loner!
	} else
	  simple_lock(&in->filelock, &need_issue);
	if (need_issue)
	  issue_caps(in);
	cap->dec_suppress();
      }
      if (!in->filelock.can_wrlock(client)) {
	in->filelock.add_waiter(SimpleLock::WAIT_STABLE, new C_MDL_CheckMaxSize(this, in));
	change_max = false;
      }
    }
  }

  if (!dirty && !change_max)
    return false;


  // do the update.
  EUpdate *le = new EUpdate(mds->mdlog, "cap update");
  mds->mdlog->start_entry(le);

  // xattrs update?
  map<string,bufferptr> *px = 0;
  if ((dirty & CEPH_CAP_XATTR_EXCL) && 
      m->xattrbl.length() &&
      m->head.xattr_version > in->get_projected_inode()->xattr_version)
    px = new map<string,bufferptr>;

  inode_t *pi = in->project_inode(px);
  pi->version = in->pre_dirty();

  Mutation *mut = new Mutation;
  mut->ls = mds->mdlog->get_current_segment();

  // file
  if (dirty & (CEPH_CAP_FILE_EXCL|CEPH_CAP_FILE_WR)) {
    utime_t atime = m->get_atime();
    utime_t mtime = m->get_mtime();
    utime_t ctime = m->get_ctime();
    uint64_t size = m->get_size();
    
    if (((dirty & CEPH_CAP_FILE_WR) && mtime > latest->mtime) ||
	((dirty & CEPH_CAP_FILE_EXCL) && mtime != latest->mtime)) {
      dout(7) << "  mtime " << pi->mtime << " -> " << mtime
	      << " for " << *in << dendl;
      pi->mtime = mtime;
    }
    if (ctime > latest->ctime) {
      dout(7) << "  ctime " << pi->ctime << " -> " << ctime
	      << " for " << *in << dendl;
      pi->ctime = ctime;
    }
    if (in->inode.is_file() &&   // ONLY if regular file
	size > latest->size) {
      dout(7) << "  size " << pi->size << " -> " << size
	      << " for " << *in << dendl;
      pi->size = size;
      pi->rstat.rbytes = size;
    }
    if ((dirty & CEPH_CAP_FILE_EXCL) && atime != latest->atime) {
      dout(7) << "  atime " << pi->atime << " -> " << atime
	      << " for " << *in << dendl;
      pi->atime = atime;
    }
    if ((dirty & CEPH_CAP_FILE_EXCL) &&
	ceph_seq_cmp(pi->time_warp_seq, m->get_time_warp_seq()) < 0) {
      dout(7) << "  time_warp_seq " << pi->time_warp_seq << " -> " << m->get_time_warp_seq()
	      << " for " << *in << dendl;
      pi->time_warp_seq = m->get_time_warp_seq();
    }
  }
  if (change_max) {
    dout(7) << "  max_size " << old_max << " -> " << new_max
	    << " for " << *in << dendl;
    if (new_max) {
      pi->client_ranges[client].first = 0;
      pi->client_ranges[client].last = new_max;
    } else 
      pi->client_ranges.erase(client);
  }
    
  if (change_max || (dirty & (CEPH_CAP_FILE_EXCL|CEPH_CAP_FILE_WR))) 
    wrlock_force(&in->filelock, mut);  // wrlock for duration of journal

  // auth
  if (dirty & CEPH_CAP_AUTH_EXCL) {
    if (m->head.uid != latest->uid) {
      dout(7) << "  uid " << pi->uid
	      << " -> " << m->head.uid
	      << " for " << *in << dendl;
      pi->uid = m->head.uid;
    }
    if (m->head.gid != latest->gid) {
      dout(7) << "  gid " << pi->gid
	      << " -> " << m->head.gid
	      << " for " << *in << dendl;
      pi->gid = m->head.gid;
    }
    if (m->head.mode != latest->mode) {
      dout(7) << "  mode " << oct << pi->mode
	      << " -> " << m->head.mode << dec
	      << " for " << *in << dendl;
      pi->mode = m->head.mode;
    }

    wrlock_force(&in->authlock, mut);
  }

  // xattr
  if (px) {
    dout(7) << " xattrs v" << pi->xattr_version << " -> " << m->head.xattr_version << dendl;
    pi->xattr_version = m->head.xattr_version;
    bufferlist::iterator p = m->xattrbl.begin();
    ::decode(*px, p);

    wrlock_force(&in->xattrlock, mut);
  }
  
  mut->auth_pin(in);
  mdcache->predirty_journal_parents(mut, &le->metablob, in, 0, PREDIRTY_PRIMARY, 0, follows);
  mdcache->journal_dirty_inode(mut, &le->metablob, in, follows);

  mds->mdlog->submit_entry(le);
  mds->mdlog->wait_for_sync(new C_Locker_FileUpdate_finish(this, in, mut, change_max, 
							   client, cap, ack));
  // only flush immediately if the lock is unstable, or unissued caps are wanted, or max_size is 
  // changing
  if (((dirty & (CEPH_CAP_FILE_EXCL|CEPH_CAP_FILE_WR)) && !in->filelock.is_stable()) ||
      ((dirty & CEPH_CAP_AUTH_EXCL) && !in->authlock.is_stable()) ||
      ((dirty & CEPH_CAP_XATTR_EXCL) && !in->xattrlock.is_stable()) ||
      (change_max && new_max) ||         // max INCREASE
      (cap->wanted() & ~cap->pending()))
    mds->mdlog->flush();

  return true;
}

void Locker::handle_client_cap_release(MClientCapRelease *m)
{
  client_t client = m->get_source().num();
  dout(10) << "handle_client_cap_release " << *m << dendl;

  for (vector<ceph_mds_cap_item>::iterator p = m->caps.begin(); p != m->caps.end(); p++) {
    inodeno_t ino((__u64)p->ino);
    CInode *in = mdcache->get_inode(ino);
    if (!in) {
      dout(10) << " missing ino " << ino << dendl;
      continue;
    }
    Capability *cap = in->get_client_cap(client);
    if (!cap) {
      dout(10) << " no cap on " << *in << dendl;
      continue;
    }
    if (cap->get_cap_id() != p->cap_id) {
      dout(7) << " ignoring client capid " << p->cap_id << " != my " << cap->get_cap_id() << " on " << *in << dendl;
      continue;
    }
    if (ceph_seq_cmp(p->migrate_seq, cap->get_mseq()) < 0) {
      dout(7) << " mseq " << p->migrate_seq << " < " << cap->get_mseq()
	      << " on " << *in << dendl;
      continue;
    }
    if (p->seq != cap->get_last_issue()) {
      dout(10) << " seq " << p->seq << " != " << cap->get_last_issue() << " on " << *in << dendl;
      
      // clean out any old revoke history
      cap->clean_revoke_from(p->seq);
      eval_cap_gather(in);
      continue;
    }

    dout(7) << "removing cap on " << *in << dendl;
    mdcache->remove_client_cap(in, client);
  }

  m->put();
}

void Locker::handle_client_lease(MClientLease *m)
{
  dout(10) << "handle_client_lease " << *m << dendl;

  assert(m->get_source().is_client());
  client_t client = m->get_source().num();

  CInode *in = mdcache->get_inode(m->get_ino(), m->get_last());
  if (!in) {
    dout(7) << "handle_client_lease don't have ino " << m->get_ino() << "." << m->get_last() << dendl;
    m->put();
    return;
  }
  CDentry *dn = 0;
  assert(m->get_mask() & CEPH_LOCK_DN);

  frag_t fg = in->pick_dirfrag(m->dname);
  CDir *dir = in->get_dirfrag(fg);
  if (dir) 
    dn = dir->lookup(m->dname);
  if (!dn) {
    dout(7) << "handle_client_lease don't have dn " << m->get_ino() << " " << m->dname << dendl;
    m->put();
    return;
  }
  dout(10) << " on " << *dn << dendl;

  // replica and lock
  ClientLease *l = dn->get_client_lease(client);
  if (!l) {
    dout(7) << "handle_client_lease didn't have lease for client" << client << " of " << *dn << dendl;
    m->put();
    return;
  } 

  switch (m->get_action()) {
  case CEPH_MDS_LEASE_REVOKE_ACK:
  case CEPH_MDS_LEASE_RELEASE:
    if (l->seq != m->get_seq()) {
      dout(7) << "handle_client_lease release - seq " << l->seq << " != provided " << m->get_seq() << dendl;
    } else {
      dout(7) << "handle_client_lease client" << client
	      << " release mask " << m->get_mask()
	      << " on " << *dn << dendl;
      int left = dn->remove_client_lease(l, l->mask, this);
      dout(10) << " remaining mask is " << left << " on " << *dn << dendl;
    }
    m->put();
    break;

  case CEPH_MDS_LEASE_RENEW:
    {
      dout(7) << "handle_client_lease client" << client
	      << " renew mask " << m->get_mask()
	      << " on " << *dn << dendl;
      int pool = 1;   // fixme.. do something smart!
      m->h.duration_ms = (int)(1000 * mdcache->client_lease_durations[pool]);
      m->h.seq = ++l->seq;
      m->clear_payload();
      
      utime_t now = g_clock.now();
      now += mdcache->client_lease_durations[pool];
      mdcache->touch_client_lease(l, pool, now);
      
      mds->send_message_client_counted(m, m->get_connection());
    }
    break;

  default:
    assert(0); // implement me
    break;
  }
}



void Locker::_issue_client_lease(CDentry *dn, int mask, int pool, client_t client,
				 bufferlist &bl, utime_t now, Session *session)
{
  LeaseStat e;
  e.mask = mask;

  if (mask) {
    ClientLease *l = dn->add_client_lease(client, mask);
    session->touch_lease(l);

    e.seq = ++l->seq;

    now += mdcache->client_lease_durations[pool];
    mdcache->touch_client_lease(l, pool, now);
  } else
    e.seq = 0;

  e.duration_ms = (int)(1000 * mdcache->client_lease_durations[pool]);
  ::encode(e, bl);
  dout(20) << "_issue_client_lease mask " << e.mask << " dur " << e.duration_ms << "ms" << dendl;
}
  


/*
int Locker::issue_client_lease(CInode *in, client_t client, 
			       bufferlist &bl, utime_t now, Session *session)
{
  int mask = CEPH_LOCK_INO;
  int pool = 1;   // fixme.. do something smart!
  if (in->authlock.can_lease()) mask |= CEPH_LOCK_IAUTH;
  if (in->linklock.can_lease()) mask |= CEPH_LOCK_ILINK;
  if (in->filelock.can_lease()) mask |= CEPH_LOCK_IFILE;
  if (in->xattrlock.can_lease()) mask |= CEPH_LOCK_IXATTR;

  _issue_client_lease(in, mask, pool, client, bl, now, session);
  return mask;
}
*/

int Locker::issue_client_lease(CDentry *dn, client_t client,
			       bufferlist &bl, utime_t now, Session *session)
{
  int pool = 1;   // fixme.. do something smart!

  // is it necessary?
  // -> dont issue per-dentry lease if a dir lease is possible, or
  //    if the client is holding EXCL|RDCACHE caps.
  int mask = 0;

  CInode *diri = dn->get_dir()->get_inode();
  if (!diri->is_stray() &&  // do not issue dn leases in stray dir!
      ((!diri->filelock.can_lease(client) &&
	(diri->get_client_cap_pending(client) & (CEPH_CAP_FILE_SHARED | CEPH_CAP_FILE_EXCL)) == 0)) &&
      dn->lock.can_lease(client))
    mask |= CEPH_LOCK_DN;
  
  _issue_client_lease(dn, mask, pool, client, bl, now, session);
  return mask;
}


void Locker::revoke_client_leases(SimpleLock *lock)
{
  int n = 0;
  CDentry *dn = (CDentry*)lock->get_parent();
  for (map<client_t, ClientLease*>::iterator p = dn->client_lease_map.begin();
       p != dn->client_lease_map.end();
       p++) {
    ClientLease *l = p->second;
    
    if ((l->mask & lock->get_type()) == 0)
      continue;
    
    n++;
    assert(lock->get_type() == CEPH_LOCK_DN);

    CDentry *dn = (CDentry*)lock->get_parent();
    int mask = CEPH_LOCK_DN;
    
    // i should also revoke the dir ICONTENT lease, if they have it!
    CInode *diri = dn->get_dir()->get_inode();
    mds->send_message_client_counted(new MClientLease(CEPH_MDS_LEASE_REVOKE, l->seq,
					      mask,
					      diri->ino(),
					      diri->first, CEPH_NOSNAP,
					      dn->get_name()),
			     l->client);
  }
  assert(n == lock->get_num_client_lease());
}



// locks ----------------------------------------------------------------

SimpleLock *Locker::get_lock(int lock_type, MDSCacheObjectInfo &info) 
{
  switch (lock_type) {
  case CEPH_LOCK_DN:
    {
      // be careful; info.dirfrag may have incorrect frag; recalculate based on dname.
      CInode *diri = mdcache->get_inode(info.dirfrag.ino);
      frag_t fg;
      CDir *dir = 0;
      CDentry *dn = 0;
      if (diri) {
	fg = diri->pick_dirfrag(info.dname);
	dir = diri->get_dirfrag(fg);
	if (dir) 
	  dn = dir->lookup(info.dname, info.snapid);
      }
      if (!dn) {
	dout(7) << "get_lock don't have dn " << info.dirfrag.ino << " " << info.dname << dendl;
	return 0;
      }
      return &dn->lock;
    }

  case CEPH_LOCK_IAUTH:
  case CEPH_LOCK_ILINK:
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_IFILE:
  case CEPH_LOCK_INEST:
  case CEPH_LOCK_IXATTR:
  case CEPH_LOCK_ISNAP:
  case CEPH_LOCK_IFLOCK:
    {
      CInode *in = mdcache->get_inode(info.ino, info.snapid);
      if (!in) {
	dout(7) << "get_lock don't have ino " << info.ino << dendl;
	return 0;
      }
      switch (lock_type) {
      case CEPH_LOCK_IAUTH: return &in->authlock;
      case CEPH_LOCK_ILINK: return &in->linklock;
      case CEPH_LOCK_IDFT: return &in->dirfragtreelock;
      case CEPH_LOCK_IFILE: return &in->filelock;
      case CEPH_LOCK_INEST: return &in->nestlock;
      case CEPH_LOCK_IXATTR: return &in->xattrlock;
      case CEPH_LOCK_ISNAP: return &in->snaplock;
      case CEPH_LOCK_IFLOCK: return &in->flocklock;
      }
    }

  default:
    dout(7) << "get_lock don't know lock_type " << lock_type << dendl;
    assert(0);
    break;
  }

  return 0;  
}


void Locker::handle_lock(MLock *m)
{
  // nobody should be talking to us during recovery.
  assert(mds->is_rejoin() || mds->is_clientreplay() || mds->is_active() || mds->is_stopping());

  SimpleLock *lock = get_lock(m->get_lock_type(), m->get_object_info());
  if (!lock) {
    dout(10) << "don't have object " << m->get_object_info() << ", must have trimmed, dropping" << dendl;
    m->put();
    return;
  }

  switch (lock->get_type()) {
  case CEPH_LOCK_DN:
  case CEPH_LOCK_IAUTH:
  case CEPH_LOCK_ILINK:
  case CEPH_LOCK_ISNAP:
  case CEPH_LOCK_IXATTR:
  case CEPH_LOCK_IFLOCK:
    handle_simple_lock(lock, m);
    break;
    
  case CEPH_LOCK_IDFT:
  case CEPH_LOCK_INEST:
    //handle_scatter_lock((ScatterLock*)lock, m);
    //break;

  case CEPH_LOCK_IFILE:
    handle_file_lock((ScatterLock*)lock, m);
    break;
    
  default:
    dout(7) << "handle_lock got otype " << m->get_lock_type() << dendl;
    assert(0);
    break;
  }
}
 




// ==========================================================================
// simple lock

void Locker::handle_simple_lock(SimpleLock *lock, MLock *m)
{
  int from = m->get_asker();
  
  dout(10) << "handle_simple_lock " << *m
	   << " on " << *lock << " " << *lock->get_parent() << dendl;

  if (mds->is_rejoin()) {
    if (lock->get_parent()->is_rejoining()) {
      dout(7) << "handle_simple_lock still rejoining " << *lock->get_parent()
	      << ", dropping " << *m << dendl;
      m->put();
      return;
    }
  }

  switch (m->get_action()) {
    // -- replica --
  case LOCK_AC_SYNC:
    assert(lock->get_state() == LOCK_LOCK);
    lock->decode_locked_state(m->get_data());
    lock->set_state(LOCK_SYNC);
    lock->finish_waiters(SimpleLock::WAIT_RD|SimpleLock::WAIT_STABLE);
    break;
    
  case LOCK_AC_LOCK:
    assert(lock->get_state() == LOCK_SYNC);
    //||           lock->get_state() == LOCK_SYNC_LOCK);
    
    // wait for readers to finish?
    if (lock->is_rdlocked() ||
	lock->get_num_client_lease()) {
      dout(7) << "handle_simple_lock has reader|leases, waiting before ack on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      revoke_client_leases(lock);
      lock->set_state(LOCK_SYNC_LOCK);
    } else {
      // update lock and reply
      lock->set_state(LOCK_LOCK);
      mds->send_message(new MLock(lock, LOCK_AC_LOCKACK, mds->get_nodeid()), m->get_connection());
    }
    break;


    // -- auth --
  case LOCK_AC_LOCKACK:
    assert(lock->get_state() == LOCK_SYNC_LOCK ||
	   lock->get_state() == LOCK_SYNC_EXCL);
    assert(lock->is_gathering(from));
    lock->remove_gather(from);
    
    if (lock->is_gathering()) {
      dout(7) << "handle_simple_lock " << *lock << " on " << *lock->get_parent() << " from " << from
	      << ", still gathering " << lock->get_gather_set() << dendl;
    } else {
      dout(7) << "handle_simple_lock " << *lock << " on " << *lock->get_parent() << " from " << from
	      << ", last one" << dendl;
      eval_gather(lock);
    }
    break;

  }

  m->put();
}

/* unused, currently.

class C_Locker_SimpleEval : public Context {
  Locker *locker;
  SimpleLock *lock;
public:
  C_Locker_SimpleEval(Locker *l, SimpleLock *lk) : locker(l), lock(lk) {}
  void finish(int r) {
    locker->try_simple_eval(lock);
  }
};

void Locker::try_simple_eval(SimpleLock *lock)
{
  // unstable and ambiguous auth?
  if (!lock->is_stable() &&
      lock->get_parent()->is_ambiguous_auth()) {
    dout(7) << "simple_eval not stable and ambiguous auth, waiting on " << *lock->get_parent() << dendl;
    //if (!lock->get_parent()->is_waiter(MDSCacheObject::WAIT_SINGLEAUTH))
    lock->get_parent()->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, new C_Locker_SimpleEval(this, lock));
    return;
  }

  if (!lock->get_parent()->is_auth()) {
    dout(7) << "try_simple_eval not auth for " << *lock->get_parent() << dendl;
    return;
  }

  if (!lock->get_parent()->can_auth_pin()) {
    dout(7) << "try_simple_eval can't auth_pin, waiting on " << *lock->get_parent() << dendl;
    //if (!lock->get_parent()->is_waiter(MDSCacheObject::WAIT_SINGLEAUTH))
    lock->get_parent()->add_waiter(MDSCacheObject::WAIT_UNFREEZE, new C_Locker_SimpleEval(this, lock));
    return;
  }

  if (lock->is_stable())
    simple_eval(lock);
}
*/


void Locker::simple_eval(SimpleLock *lock, bool *need_issue)
{
  dout(10) << "simple_eval " << *lock << " on " << *lock->get_parent() << dendl;

  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  if (lock->get_parent()->is_frozen()) return;

  CInode *in = 0;
  int wanted = 0, issued = 0;
  if (lock->get_type() != CEPH_LOCK_DN) {
    in = (CInode*)lock->get_parent();
    wanted = in->get_caps_wanted(NULL, NULL, lock->get_cap_shift());
    issued = in->get_caps_issued(NULL, NULL, NULL, lock->get_cap_shift());
  }
  
  // -> excl?
  if (lock->get_state() != LOCK_EXCL &&
      in && in->get_target_loner() >= 0 &&
      (wanted & CEPH_CAP_GEXCL)) {
    dout(7) << "simple_eval stable, going to excl " << *lock 
	    << " on " << *lock->get_parent() << dendl;
    simple_excl(lock, need_issue);
  }

  // stable -> sync?
  else if (lock->get_state() != LOCK_SYNC &&
	   !lock->is_xlocked() &&
	   !lock->is_wrlocked() &&
	   ((!(issued & CEPH_CAP_GEXCL) &&
	     !lock->is_waiter_for(SimpleLock::WAIT_WR)) ||
	    (lock->get_state() == LOCK_EXCL && in && in->get_target_loner() < 0))) {
    dout(7) << "simple_eval stable, syncing " << *lock 
	    << " on " << *lock->get_parent() << dendl;
    simple_sync(lock, need_issue);
  }
}


// mid

bool Locker::simple_sync(SimpleLock *lock, bool *need_issue)
{
  dout(7) << "simple_sync on " << *lock << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  CInode *in = 0;
  if (lock->get_cap_shift())
    in = (CInode *)lock->get_parent();

  int old_state = lock->get_state();

  if (old_state != LOCK_TSYN) {

    switch (lock->get_state()) {
    case LOCK_MIX: lock->set_state(LOCK_MIX_SYNC); break;
    case LOCK_SCAN:
    case LOCK_LOCK: lock->set_state(LOCK_LOCK_SYNC); break;
    case LOCK_EXCL: lock->set_state(LOCK_EXCL_SYNC); break;
    default: assert(0);
    }

    int gather = 0;
    if (lock->is_wrlocked())
      gather++;
    if (lock->is_xlocked())
      gather++;
    
    if (lock->get_parent()->is_replicated() && 
	old_state == LOCK_MIX) {
      send_lock_message(lock, LOCK_AC_SYNC);
      lock->init_gather();
      gather++;
    }
    
    if (in) {
      if (in->issued_caps_need_gather(lock)) {
	if (need_issue)
	  *need_issue = true;
	else
	  issue_caps(in);
	gather++;
      }
    }
    
    if (lock->get_type() == CEPH_LOCK_IFILE &&
	in->state_test(CInode::STATE_NEEDSRECOVER)) {
      mds->mdcache->queue_file_recover(in);
      mds->mdcache->do_file_recover();
      gather++;
    }
    
    if (!gather && lock->is_updated()) {
      lock->get_parent()->auth_pin(lock);
      scatter_writebehind((ScatterLock*)lock);
      mds->mdlog->flush();
      return false;
    }

    if (gather) {
      lock->get_parent()->auth_pin(lock);
      return false;
    }
  }

  if (lock->get_parent()->is_replicated()) {
    bufferlist data;
    lock->encode_locked_state(data);
    send_lock_message(lock, LOCK_AC_SYNC, data);
  }
  lock->set_state(LOCK_SYNC);
  lock->finish_waiters(SimpleLock::WAIT_RD|SimpleLock::WAIT_STABLE);
  return true;
}

void Locker::simple_excl(SimpleLock *lock, bool *need_issue)
{
  dout(7) << "simple_excl on " << *lock << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  CInode *in = 0;
  if (lock->get_cap_shift())
    in = (CInode *)lock->get_parent();

  switch (lock->get_state()) {
  case LOCK_SCAN:
  case LOCK_LOCK: lock->set_state(LOCK_LOCK_EXCL); break;
  case LOCK_SYNC: lock->set_state(LOCK_SYNC_EXCL); break;
  default: assert(0);
  }
  
  int gather = 0;
  if (lock->is_wrlocked())
    gather++;
  if (lock->is_xlocked())
    gather++;
  
  if (lock->get_parent()->is_replicated() && 
      lock->get_state() != LOCK_LOCK_EXCL) {
    send_lock_message(lock, LOCK_AC_LOCK);
    lock->init_gather();
    gather++;
  }
  
  if (in) {
    if (in->issued_caps_need_gather(lock)) {
      if (need_issue)
	*need_issue = true;
      else
	issue_caps(in);
      gather++;
    }
  }
  
  if (gather) {
    lock->get_parent()->auth_pin(lock);
  } else {
    lock->set_state(LOCK_EXCL);
    lock->finish_waiters(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE);
  }
}


void Locker::simple_lock(SimpleLock *lock, bool *need_issue)
{
  dout(7) << "simple_lock on " << *lock << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());
  assert(lock->get_state() != LOCK_LOCK);
  
  CInode *in = 0;
  if (lock->get_cap_shift())
    in = (CInode *)lock->get_parent();

  int old_state = lock->get_state();

  switch (lock->get_state()) {
  case LOCK_SCAN: lock->set_state(LOCK_SCAN_LOCK); break;
  case LOCK_SYNC: lock->set_state(LOCK_SYNC_LOCK); break;
  case LOCK_EXCL: lock->set_state(LOCK_EXCL_LOCK); break;
  case LOCK_MIX: lock->set_state(LOCK_MIX_LOCK); break;
  case LOCK_TSYN: lock->set_state(LOCK_TSYN_LOCK); break;
  default: assert(0);
  }

  int gather = 0;
  if (lock->get_parent()->is_replicated() &&
      lock->get_sm()->states[old_state].replica_state != LOCK_LOCK) {  // replica may already be LOCK
    gather++;
    send_lock_message(lock, LOCK_AC_LOCK);
    lock->init_gather();
  }
  if (lock->get_num_client_lease()) {
    gather++;
    revoke_client_leases(lock);
  }
  if (lock->is_rdlocked())
    gather++;
  if (in) {
    if (in->issued_caps_need_gather(lock)) {
      if (need_issue)
	*need_issue = true;
      else
	issue_caps(in);
      gather++;
    }
  }

  if (lock->get_type() == CEPH_LOCK_IFILE &&
      in->state_test(CInode::STATE_NEEDSRECOVER)) {
    mds->mdcache->queue_file_recover(in);
    mds->mdcache->do_file_recover();
    gather++;
  }

  if (!gather && lock->is_updated()) {
    lock->get_parent()->auth_pin(lock);
    scatter_writebehind((ScatterLock*)lock);
    mds->mdlog->flush();
    return;
  }

  if (gather) {
    lock->get_parent()->auth_pin(lock);
  } else {
    lock->set_state(LOCK_LOCK);
    lock->finish_waiters(ScatterLock::WAIT_XLOCK|ScatterLock::WAIT_WR|ScatterLock::WAIT_STABLE);
  }
}


void Locker::simple_xlock(SimpleLock *lock)
{
  dout(7) << "simple_xlock on " << *lock << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  //assert(lock->is_stable());
  assert(lock->get_state() != LOCK_XLOCK);
  
  CInode *in = 0;
  if (lock->get_cap_shift())
    in = (CInode *)lock->get_parent();

  if (lock->is_stable())
    lock->get_parent()->auth_pin(lock);

  switch (lock->get_state()) {
  case LOCK_LOCK: 
  case LOCK_XLOCKDONE: lock->set_state(LOCK_LOCK_XLOCK); break;
  default: assert(0);
  }

  int gather = 0;
  if (lock->is_rdlocked())
    gather++;
  if (lock->is_wrlocked())
    gather++;
  
  if (in) {
    if (in->issued_caps_need_gather(lock)) {
      issue_caps(in);
      gather++;
    }
  }

  if (!gather) {
    lock->set_state(LOCK_PREXLOCK);
  }
}




// dentry specific helpers

/** dentry_can_rdlock_trace
 * see if we can _anonymously_ rdlock an entire trace.  
 * if not, and req is specified, wait and retry that message.
 */
bool Locker::dentry_can_rdlock_trace(vector<CDentry*>& trace) 
{
  // verify dentries are rdlockable.
  // we do this because
  // - we're being less aggressive about locks acquisition, and
  // - we're not acquiring the locks in order!
  for (vector<CDentry*>::iterator it = trace.begin();
       it != trace.end();
       it++) {
    CDentry *dn = *it;
    if (!dn->lock.can_rdlock(-1)) {
      dout(10) << "can_rdlock_trace can't rdlock " << *dn << dendl;
      return false;
    }
  }
  return true;
}

void Locker::dentry_anon_rdlock_trace_start(vector<CDentry*>& trace)
{
  // grab dentry rdlocks
  for (vector<CDentry*>::iterator it = trace.begin();
       it != trace.end();
       it++) {
    dout(10) << "dentry_anon_rdlock_trace_start rdlocking " << (*it)->lock << " " << **it << dendl;
    (*it)->lock.get_rdlock();
  }
}


void Locker::dentry_anon_rdlock_trace_finish(vector<CDentry*>& trace)
{
  for (vector<CDentry*>::iterator it = trace.begin();
       it != trace.end();
       it++) 
    rdlock_finish(&(*it)->lock, 0);
}



// ==========================================================================
// scatter lock

void Locker::scatter_writebehind(ScatterLock *lock)
{
  CInode *in = (CInode*)lock->get_parent();
  dout(10) << "scatter_writebehind " << in->inode.mtime << " on " << *lock << " on " << *in << dendl;

  // journal
  Mutation *mut = new Mutation;
  mut->ls = mds->mdlog->get_current_segment();

  // forcefully take a wrlock
  lock->get_wrlock(true);
  mut->wrlocks.insert(lock);
  mut->locks.insert(lock);

  in->pre_cow_old_inode();  // avoid cow mayhem

  inode_t *pi = in->project_inode();
  pi->version = in->pre_dirty();

  lock->get_parent()->finish_scatter_gather_update(lock->get_type());
  lock->start_flush();

  EUpdate *le = new EUpdate(mds->mdlog, "scatter_writebehind");
  mds->mdlog->start_entry(le);

  mdcache->predirty_journal_parents(mut, &le->metablob, in, 0, PREDIRTY_PRIMARY, false);
  mdcache->journal_dirty_inode(mut, &le->metablob, in);
  
  mds->mdlog->submit_entry(le);
  mds->mdlog->wait_for_sync(new C_Locker_ScatterWB(this, lock, mut));
}

void Locker::scatter_writebehind_finish(ScatterLock *lock, Mutation *mut)
{
  CInode *in = (CInode*)lock->get_parent();
  dout(10) << "scatter_writebehind_finish on " << *lock << " on " << *in << dendl;
  in->pop_and_dirty_projected_inode(mut->ls);

  lock->finish_flush();

  mut->apply();
  drop_locks(mut);
  mut->cleanup();
  delete mut;

  if (lock->is_stable())
    lock->finish_waiters(ScatterLock::WAIT_STABLE);

  //scatter_eval_gather(lock);
}

void Locker::scatter_eval(ScatterLock *lock, bool *need_issue)
{
  dout(10) << "scatter_eval " << *lock << " on " << *lock->get_parent() << dendl;

  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  if (lock->get_parent()->is_frozen()) return;
  
  if (lock->get_type() == CEPH_LOCK_INEST) {
    // in general, we want to keep INEST scattered at all times.
    if (!lock->is_rdlocked() &&
	lock->get_state() != LOCK_MIX)
      file_mixed(lock, need_issue);
      return;
  }

  CInode *in = (CInode*)lock->get_parent();
  if (!in->has_subtree_root_dirfrag() || in->is_base()) {
    // i _should_ be sync.
    if (!lock->is_wrlocked() &&
	!lock->is_xlocked() &&
	lock->get_state() != LOCK_SYNC) {
      dout(10) << "scatter_eval no wrlocks|xlocks, not subtree root inode, syncing" << dendl;
      simple_sync(lock, need_issue);
    }
  }
}


/*
 * mark a scatterlock to indicate that the dir fnode has some dirty data
 */
void Locker::mark_updated_scatterlock(ScatterLock *lock)
{
  lock->mark_dirty();
  if (lock->get_updated_item()->is_on_list()) {
    dout(10) << "mark_updated_scatterlock " << *lock
	     << " - already on list since " << lock->get_update_stamp() << dendl;
  } else {
    updated_scatterlocks.push_back(lock->get_updated_item());
    utime_t now = g_clock.now();
    lock->set_update_stamp(now);
    dout(10) << "mark_updated_scatterlock " << *lock
	     << " - added at " << now << dendl;
  }
}

/*
 * this is called by scatter_tick and LogSegment::try_to_trim() when
 * trying to flush dirty scattered data (i.e. updated fnode) back to
 * the inode.
 *
 * we need to lock|scatter in order to push fnode changes into the
 * inode.dirstat.
 */
void Locker::scatter_nudge(ScatterLock *lock, Context *c, bool forcelockchange)
{
  CInode *p = (CInode *)lock->get_parent();

  if (p->is_frozen() || p->is_freezing()) {
    dout(10) << "scatter_nudge waiting for unfreeze on " << *p << dendl;
    if (c) 
      p->add_waiter(MDSCacheObject::WAIT_UNFREEZE, c);
    else
      // just requeue.  not ideal.. starvation prone..
      updated_scatterlocks.push_back(lock->get_updated_item());
    return;
  }

  if (p->is_ambiguous_auth()) {
    dout(10) << "scatter_nudge waiting for single auth on " << *p << dendl;
    if (c) 
      p->add_waiter(MDSCacheObject::WAIT_SINGLEAUTH, c);
    else
      // just requeue.  not ideal.. starvation prone..
      updated_scatterlocks.push_back(lock->get_updated_item());
    return;
  }

  if (p->is_auth()) {
    while (true) {
      if (lock->is_stable()) {
	// can we do it now?
	//  (only if we're not replicated.. if we are, we really do need
	//   to nudge the lock state!)
	if (!forcelockchange && lock->can_wrlock(-1)) {
	  dout(10) << "scatter_nudge auth, propagating " << *lock << " on " << *p << dendl;
	  scatter_writebehind(lock);
	  if (c)
	    lock->add_waiter(SimpleLock::WAIT_STABLE, c);
	  return;
	}

	// adjust lock state
	dout(10) << "scatter_nudge auth, scatter/unscattering " << *lock << " on " << *p << dendl;
	switch (lock->get_type()) {
	case CEPH_LOCK_IFILE:
	  if (p->is_replicated() && lock->get_state() != LOCK_MIX)
	    file_mixed((ScatterLock*)lock);
	  else if (lock->get_state() != LOCK_LOCK)
	    simple_lock((ScatterLock*)lock);
	  else
	    simple_sync((ScatterLock*)lock);
	  break;
	  
	case CEPH_LOCK_IDFT:
	case CEPH_LOCK_INEST:
	  if (p->is_replicated() && lock->get_state() != LOCK_MIX)
	    file_mixed(lock);
	  else if (lock->get_state() != LOCK_LOCK)
	    simple_lock(lock);
	  else
	    simple_sync(lock);
	  break;
	default:
	  assert(0);
	}
      } else {
	dout(10) << "scatter_nudge auth, waiting for stable " << *lock << " on " << *p << dendl;
	if (c)
	  lock->add_waiter(SimpleLock::WAIT_STABLE, c);
	return;
      }
    }
  } else {
    dout(10) << "scatter_nudge replica, requesting scatter/unscatter of " 
	     << *lock << " on " << *p << dendl;
    // request unscatter?
    int auth = lock->get_parent()->authority().first;
    if (mds->mdsmap->get_state(auth) >= MDSMap::STATE_ACTIVE)
      mds->send_message_mds(new MLock(lock, LOCK_AC_NUDGE, mds->get_nodeid()), auth);

    // wait...
    if (c)
      lock->add_waiter(SimpleLock::WAIT_STABLE, c);

    // also, requeue, in case we had wrong auth or something
    updated_scatterlocks.push_back(lock->get_updated_item());
  }
}

void Locker::scatter_tick()
{
  dout(10) << "scatter_tick" << dendl;
  
  // updated
  utime_t now = g_clock.now();
  int n = updated_scatterlocks.size();
  while (!updated_scatterlocks.empty()) {
    ScatterLock *lock = updated_scatterlocks.front();

    if (n-- == 0) break;  // scatter_nudge() may requeue; avoid looping
    
    if (!lock->is_updated()) {
      updated_scatterlocks.pop_front();
      dout(10) << " removing from updated_scatterlocks " 
	       << *lock << " " << *lock->get_parent() << dendl;
      continue;
    }
    if (now - lock->get_update_stamp() < g_conf.mds_scatter_nudge_interval)
      break;
    updated_scatterlocks.pop_front();
    scatter_nudge(lock, 0);
  }
  mds->mdlog->flush();
}


void Locker::scatter_tempsync(ScatterLock *lock, bool *need_issue)
{
  dout(10) << "scatter_tempsync " << *lock
	   << " on " << *lock->get_parent() << dendl;
  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  CInode *in = (CInode *)lock->get_parent();

  switch (lock->get_state()) {
  case LOCK_SYNC: assert(0);   // this shouldn't happen
  case LOCK_LOCK: lock->set_state(LOCK_LOCK_TSYN); break;
  case LOCK_MIX: lock->set_state(LOCK_MIX_TSYN); break;
  default: assert(0);
  }

  int gather = 0;
  if (lock->is_wrlocked())
    gather++;
  if (lock->is_xlocked())
    gather++;

  if (in->issued_caps_need_gather(lock)) {
    if (need_issue)
      *need_issue = true;
    else
      issue_caps(in);
    gather++;
  }

  if (lock->get_state() == LOCK_MIX_TSYN &&
      in->is_replicated()) {
    lock->init_gather();
    send_lock_message(lock, LOCK_AC_LOCK);
    gather++;
  }

  if (gather) {
    in->auth_pin(lock);
  } else {
    // do tempsync
    lock->set_state(LOCK_TSYN);
    lock->finish_waiters(ScatterLock::WAIT_RD|ScatterLock::WAIT_STABLE);
  }
}



// ==========================================================================
// local lock

void Locker::local_wrlock_grab(LocalLock *lock, Mutation *mut)
{
  dout(7) << "local_wrlock_grab  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  
  assert(lock->can_wrlock());
  assert(!mut->wrlocks.count(lock));
  lock->get_wrlock(mut->get_client());
  mut->wrlocks.insert(lock);
  mut->locks.insert(lock);
}

bool Locker::local_wrlock_start(LocalLock *lock, MDRequest *mut)
{
  dout(7) << "local_wrlock_start  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  
  if (lock->can_wrlock()) {
    assert(!mut->wrlocks.count(lock));
    lock->get_wrlock(mut->get_client());
    mut->wrlocks.insert(lock);
    mut->locks.insert(lock);
    return true;
  } else {
    lock->add_waiter(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
    return false;
  }
}

void Locker::local_wrlock_finish(LocalLock *lock, Mutation *mut)
{
  dout(7) << "local_wrlock_finish  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  lock->put_wrlock();
  mut->wrlocks.erase(lock);
  mut->locks.erase(lock);
}

bool Locker::local_xlock_start(LocalLock *lock, MDRequest *mut)
{
  dout(7) << "local_xlock_start  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  
  if (!lock->can_xlock(-1)) {
    lock->add_waiter(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE, new C_MDS_RetryRequest(mdcache, mut));
    return false;
  }

  lock->get_xlock(mut, mut->get_client());
  mut->xlocks.insert(lock);
  mut->locks.insert(lock);
  return true;
}

void Locker::local_xlock_finish(LocalLock *lock, Mutation *mut)
{
  dout(7) << "local_xlock_finish  on " << *lock
	  << " on " << *lock->get_parent() << dendl;  
  lock->put_xlock();
  mut->xlocks.erase(lock);
  mut->locks.erase(lock);

  lock->finish_waiters(SimpleLock::WAIT_STABLE | 
		       SimpleLock::WAIT_WR | 
		       SimpleLock::WAIT_RD);
}



// ==========================================================================
// file lock


void Locker::file_eval(ScatterLock *lock, bool *need_issue)
{
  CInode *in = (CInode*)lock->get_parent();
  int loner_wanted, other_wanted;
  int wanted = in->get_caps_wanted(&loner_wanted, &other_wanted, CEPH_CAP_SFILE);
  dout(7) << "file_eval wanted=" << gcap_string(wanted)
	  << " loner_wanted=" << gcap_string(loner_wanted)
	  << " other_wanted=" << gcap_string(other_wanted)
	  << "  filelock=" << *lock << " on " << *lock->get_parent()
	  << dendl;

  assert(lock->get_parent()->is_auth());
  assert(lock->is_stable());

  if (lock->is_xlocked() || 
      lock->get_parent()->is_frozen()) return;

  // excl -> *?
  if (lock->get_state() == LOCK_EXCL) {
    dout(20) << " is excl" << dendl;
    int loner_issued, other_issued, xlocker_issued;
    in->get_caps_issued(&loner_issued, &other_issued, &xlocker_issued, CEPH_CAP_SFILE);

    if (!((loner_wanted|loner_issued) & (CEPH_CAP_GEXCL|CEPH_CAP_GWR|CEPH_CAP_GBUFFER)) ||
	 (other_wanted & (CEPH_CAP_GEXCL|CEPH_CAP_GWR|CEPH_CAP_GBUFFER|CEPH_CAP_GRD|CEPH_CAP_GCACHE)) ||
	(in->inode.is_dir() && in->multiple_nonstale_caps())) {  // FIXME.. :/
      dout(20) << " should lose it" << dendl;
      // we should lose it.
      if ((other_wanted & (CEPH_CAP_GRD|CEPH_CAP_GWR)) ||
	  lock->is_waiter_for(SimpleLock::WAIT_WR))
	file_mixed(lock, need_issue);
      else if (!lock->is_wrlocked())   // let excl wrlocks drain first
	simple_sync(lock, need_issue);
      else
	dout(10) << " waiting for wrlock to drain" << dendl;
    }    
  }
  
  // * -> excl?
  else if (lock->get_state() != LOCK_EXCL &&
	   //!lock->is_rdlocked() &&
	   //!lock->is_waiter_for(SimpleLock::WAIT_WR) &&
	   ((wanted & (CEPH_CAP_GWR|CEPH_CAP_GBUFFER)) ||
	    (in->inode.is_dir() && !in->has_subtree_root_dirfrag())) &&
	   in->get_target_loner() >= 0) {
    dout(7) << "file_eval stable, bump to loner " << *lock
	    << " on " << *lock->get_parent() << dendl;
    file_excl(lock, need_issue);
  }

  // * -> mixed?
  else if (lock->get_state() != LOCK_MIX &&
	   //!lock->is_rdlocked() &&
	   //!lock->is_waiter_for(SimpleLock::WAIT_WR) &&
	   (lock->get_scatter_wanted() ||
	    (in->multiple_nonstale_caps() && (wanted & CEPH_CAP_GWR)))) {
    dout(7) << "file_eval stable, bump to mixed " << *lock
	    << " on " << *lock->get_parent() << dendl;
    file_mixed(lock, need_issue);
  }
  
  // * -> sync?
  else if (lock->get_state() != LOCK_SYNC &&
	   !lock->is_wrlocked() &&   // drain wrlocks first!
	   !in->filelock.is_waiter_for(SimpleLock::WAIT_WR) &&
	   !(wanted & (CEPH_CAP_GWR|CEPH_CAP_GBUFFER)) &&
	   !(in->get_state() == LOCK_MIX &&
	     in->is_dir() && in->has_subtree_root_dirfrag())  // if we are a delegation point, stay where we are
	   //((wanted & CEPH_CAP_RD) || 
	   //in->is_replicated() || 
	   //lock->get_num_client_lease() || 
	   //(!loner && lock->get_state() == LOCK_EXCL)) &&
	   ) {
    dout(7) << "file_eval stable, bump to sync " << *lock 
	    << " on " << *lock->get_parent() << dendl;
    simple_sync(lock, need_issue);
  }
}



void Locker::file_mixed(ScatterLock *lock, bool *need_issue)
{
  dout(7) << "file_mixed " << *lock << " on " << *lock->get_parent() << dendl;  

  CInode *in = (CInode*)lock->get_parent();
  assert(in->is_auth());
  assert(lock->is_stable());

  if (lock->get_state() == LOCK_LOCK) {
    if (in->is_replicated()) {
      // data
      bufferlist softdata;
      lock->encode_locked_state(softdata);
      
      // bcast to replicas
      send_lock_message(lock, LOCK_AC_MIX, softdata);
    }

    // change lock
    lock->set_state(LOCK_MIX);
    lock->clear_scatter_wanted();
    if (need_issue)
      *need_issue = true;
    else
      issue_caps(in);
  } else {
    // gather?
    switch (lock->get_state()) {
    case LOCK_SYNC: lock->set_state(LOCK_SYNC_MIX); break;
    case LOCK_EXCL: lock->set_state(LOCK_EXCL_MIX); break;
    case LOCK_TSYN: lock->set_state(LOCK_TSYN_MIX); break;
    default: assert(0);
    }

    int gather = 0;
    if (in->is_replicated()) {
      if (lock->get_state() != LOCK_EXCL_MIX) {  // EXCL replica is already LOCK
	send_lock_message(lock, LOCK_AC_MIX);
	lock->init_gather();
	gather++;
      }
    }
    if (lock->get_num_client_lease()) {
      revoke_client_leases(lock);
      gather++;
    }
    if (lock->get_cap_shift()) {
      if (in->issued_caps_need_gather(lock)) {
	if (need_issue)
	  *need_issue = true;
	else
	  issue_caps(in);
	gather++;
      }
    }
    if (in->state_test(CInode::STATE_NEEDSRECOVER)) {
      mds->mdcache->queue_file_recover(in);
      mds->mdcache->do_file_recover();
      gather++;
    }

    if (gather)
      lock->get_parent()->auth_pin(lock);
    else {
      lock->set_state(LOCK_MIX);
      lock->clear_scatter_wanted();
      if (in->is_replicated()) {
	bufferlist softdata;
	lock->encode_locked_state(softdata);
	send_lock_message(lock, LOCK_AC_MIX, softdata);
      }
      if (lock->get_cap_shift()) {
	if (need_issue)
	  *need_issue = true;
	else
	  issue_caps(in);
      }
    }
  }
}


void Locker::file_excl(ScatterLock *lock, bool *need_issue)
{
  CInode *in = (CInode*)lock->get_parent();
  dout(7) << "file_excl " << *lock << " on " << *lock->get_parent() << dendl;  

  assert(in->is_auth());
  assert(lock->is_stable());

  assert(in->get_loner() >= 0 && in->mds_caps_wanted.empty());
  
  switch (lock->get_state()) {
  case LOCK_SYNC: lock->set_state(LOCK_SYNC_EXCL); break;
  case LOCK_MIX: lock->set_state(LOCK_MIX_EXCL); break;
  case LOCK_SCAN:
  case LOCK_LOCK: lock->set_state(LOCK_LOCK_EXCL); break;
  default: assert(0);
  }
  int gather = 0;
  
  if (in->is_replicated() &&
      lock->get_state() != LOCK_LOCK_EXCL) {  // if we were lock, replicas are already lock.
    send_lock_message(lock, LOCK_AC_LOCK);
    lock->init_gather();
    gather++;
  }
  if (lock->get_num_client_lease()) {
    revoke_client_leases(lock);
    gather++;
  }
  if (in->issued_caps_need_gather(lock)) {
    if (need_issue)
      *need_issue = true;
    else
      issue_caps(in);
    gather++;
  }
  if (in->state_test(CInode::STATE_NEEDSRECOVER)) {
    mds->mdcache->queue_file_recover(in);
    mds->mdcache->do_file_recover();
    gather++;
  }
  
  if (gather) {
    lock->get_parent()->auth_pin(lock);
  } else {
    lock->set_state(LOCK_EXCL);
    if (need_issue)
      *need_issue = true;
    else
      issue_caps(in);
  }
}


void Locker::file_recover(ScatterLock *lock)
{
  CInode *in = (CInode *)lock->get_parent();
  dout(7) << "file_recover " << *lock << " on " << *in << dendl;

  assert(in->is_auth());
  //assert(lock->is_stable());
  assert(lock->get_state() == LOCK_PRE_SCAN); // only called from MDCache::start_files_to_recover()

  int gather = 0;
  
  /*
  if (in->is_replicated()
      lock->get_sm()->states[oldstate].replica_state != LOCK_LOCK) {
    send_lock_message(lock, LOCK_AC_LOCK);
    lock->init_gather();
    gather++;
  }
  */
  if (in->issued_caps_need_gather(lock)) {
    issue_caps(in);
    gather++;
  }
  if (gather) {
    lock->get_parent()->auth_pin(lock);
  } else {
    lock->set_state(LOCK_SCAN);
    mds->mdcache->queue_file_recover(in);
  }
}


// messenger

void Locker::handle_file_lock(ScatterLock *lock, MLock *m)
{
  CInode *in = (CInode*)lock->get_parent();
  int from = m->get_asker();

  if (mds->is_rejoin()) {
    if (in->is_rejoining()) {
      dout(7) << "handle_file_lock still rejoining " << *in
	      << ", dropping " << *m << dendl;
      m->put();
      return;
    }
  }

  dout(7) << "handle_file_lock a=" << get_lock_action_name(m->get_action())
	  << " on " << *lock
	  << " from mds" << from << " " 
	  << *in << dendl;

  bool caps = lock->get_cap_shift();
  
  switch (m->get_action()) {
    // -- replica --
  case LOCK_AC_SYNC:
    assert(lock->get_state() == LOCK_LOCK ||
	   lock->get_state() == LOCK_MIX ||
	   lock->get_state() == LOCK_MIX_SYNC2);
    
    if (lock->get_state() == LOCK_MIX) {
      lock->set_state(LOCK_MIX_SYNC);
      eval_gather(lock, true);
      break;
    }

    // ok
    lock->decode_locked_state(m->get_data());
    lock->set_state(LOCK_SYNC);

    ((ScatterLock *)lock)->finish_flush();

    lock->get_rdlock();
    lock->finish_waiters(SimpleLock::WAIT_RD|SimpleLock::WAIT_STABLE);
    lock->put_rdlock();
    break;
    
  case LOCK_AC_LOCK:
    switch (lock->get_state()) {
    case LOCK_SYNC: lock->set_state(LOCK_SYNC_LOCK); break;
    case LOCK_MIX: lock->set_state(LOCK_MIX_LOCK); break;
    default: assert(0);
    }

    eval_gather(lock, true);
    break;
    
  case LOCK_AC_MIX:
    assert(lock->get_state() == LOCK_SYNC ||
           lock->get_state() == LOCK_LOCK ||
	   lock->get_state() == LOCK_SYNC_MIX2);
    
    if (lock->get_state() == LOCK_SYNC) {
      // MIXED
      lock->set_state(LOCK_SYNC_MIX);
      eval_gather(lock, true);
      break;
    } 
    
    // ok
    lock->decode_locked_state(m->get_data());
    lock->set_state(LOCK_MIX);

    ((ScatterLock *)lock)->finish_flush();

    if (caps)
      issue_caps(in);
    
    lock->finish_waiters(SimpleLock::WAIT_WR|SimpleLock::WAIT_STABLE);
    break;


    // -- auth --
  case LOCK_AC_LOCKACK:
    assert(lock->get_state() == LOCK_SYNC_LOCK ||
           lock->get_state() == LOCK_MIX_LOCK ||
           lock->get_state() == LOCK_MIX_EXCL ||
           lock->get_state() == LOCK_SYNC_EXCL ||
           lock->get_state() == LOCK_SYNC_MIX ||
	   lock->get_state() == LOCK_MIX_TSYN);
    assert(lock->is_gathering(from));
    lock->remove_gather(from);
    
    if (lock->get_state() == LOCK_MIX_LOCK ||
	lock->get_state() == LOCK_MIX_EXCL ||
	lock->get_state() == LOCK_MIX_TSYN)
      lock->decode_locked_state(m->get_data());

    if (lock->is_gathering()) {
      dout(7) << "handle_file_lock " << *in << " from " << from
	      << ", still gathering " << lock->get_gather_set() << dendl;
    } else {
      dout(7) << "handle_file_lock " << *in << " from " << from
	      << ", last one" << dendl;
      eval_gather(lock);
    }
    break;
    
  case LOCK_AC_SYNCACK:
    assert(lock->get_state() == LOCK_MIX_SYNC);
    assert(lock->is_gathering(from));
    lock->remove_gather(from);
    
    lock->decode_locked_state(m->get_data());

    if (lock->is_gathering()) {
      dout(7) << "handle_file_lock " << *in << " from " << from
	      << ", still gathering " << lock->get_gather_set() << dendl;
    } else {
      dout(7) << "handle_file_lock " << *in << " from " << from
	      << ", last one" << dendl;
      eval_gather(lock);
    }
    break;

  case LOCK_AC_MIXACK:
    assert(lock->get_state() == LOCK_SYNC_MIX);
    assert(lock->is_gathering(from));
    lock->remove_gather(from);
    
    if (lock->is_gathering()) {
      dout(7) << "handle_file_lock " << *in << " from " << from
	      << ", still gathering " << lock->get_gather_set() << dendl;
    } else {
      dout(7) << "handle_file_lock " << *in << " from " << from
	      << ", last one" << dendl;
      eval_gather(lock);
    }
    break;


    // requests....
  case LOCK_AC_REQSCATTER:
    if (lock->is_stable()) {
      /* NOTE: we can do this _even_ if !can_auth_pin (i.e. freezing)
       *  because the replica should be holding an auth_pin if they're
       *  doing this (and thus, we are freezing, not frozen, and indefinite
       *  starvation isn't an issue).
       */
      dout(7) << "handle_file_lock got scatter request on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      file_mixed(lock);
    } else {
      dout(7) << "handle_file_lock ignoring scatter request on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      lock->set_scatter_wanted();
    }
    break;

  case LOCK_AC_REQRDLOCK:
    if (lock->is_stable()) {
      dout(7) << "handle_file_lock got rdlock request on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      assert(in->is_auth()); // replica auth pinned if they're doing this!
      simple_sync(lock);
    } else {
      dout(7) << "handle_file_lock ignoring rdlock request on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      // replica will retry.
    }
    break;

  case LOCK_AC_NUDGE:
    if (lock->get_parent()->is_auth()) {
      dout(7) << "handle_file_lock trying nudge on " << *lock
	      << " on " << *lock->get_parent() << dendl;
      scatter_nudge(lock, 0, true);
      mds->mdlog->flush();
    } else {
      dout(7) << "handle_file_lock IGNORING nudge on non-auth " << *lock
	      << " on " << *lock->get_parent() << dendl;
    }    
    break;

  default:
    assert(0);
  }  
  
  m->put();
}






