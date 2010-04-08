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


#ifndef __FILESTORE_H
#define __FILESTORE_H

#include "ObjectStore.h"
#include "JournalingObjectStore.h"

#include "common/WorkQueue.h"

#include "common/Mutex.h"

#include "Fake.h"
//#include "FakeStoreBDBCollections.h"

#include <signal.h>

#include <map>
#include <deque>
using namespace std;

#include <ext/hash_map>
using namespace __gnu_cxx;

// fake attributes in memory, if we need to.

class FileStore : public JournalingObjectStore {
  string basedir, journalpath;
  char current_fn[PATH_MAX];
  char current_op_seq_fn[PATH_MAX];
  __u64 fsid;
  
  bool btrfs;
  bool btrfs_trans_start_end;
  bool btrfs_clone_range;
  bool btrfs_snap_create;
  bool btrfs_snap_destroy;
  bool btrfs_snap_create_async;
  bool btrfs_snap_destroy_async;
  bool btrfs_wait_sync;
  int fsid_fd, op_fd;

  int basedir_fd, current_fd;
  deque<__u64> snaps;

  // fake attrs?
  FakeAttrs attrs;
  bool fake_attrs;

  // fake collections?
  FakeCollections collections;
  bool fake_collections;
  
  Finisher ondisk_finisher;

  // helper fns
  void append_oname(const sobject_t &oid, char *s, int len);
  //void get_oname(sobject_t oid, char *s);
  void get_cdir(coll_t cid, char *s, int len);
  void get_coname(coll_t cid, const sobject_t& oid, char *s, int len);
  bool parse_object(char *s, sobject_t& o);
  bool parse_coll(char *s, coll_t& c);
  
  int lock_fsid();

  // sync thread
  Mutex lock;
  Cond sync_cond;
  __u64 sync_epoch;
  bool stop;
  void sync_entry();
  struct SyncThread : public Thread {
    FileStore *fs;
    SyncThread(FileStore *f) : fs(f) {}
    void *entry() {
      fs->sync_entry();
      return 0;
    }
  } sync_thread;

  void sync_fs(); // actuall sync underlying fs

  // -- op workqueue --
  struct Op {
    __u64 op;
    list<Transaction*> tls;
    Context *onreadable, *onreadable_sync;
    __u64 ops, bytes;
  };
  struct OpSequencer {
    Sequencer *parent;
    list<Op*> q;
    Mutex lock;
    OpSequencer() : lock("FileStore::OpSequencer::lock", false, false) {}
  };
  Sequencer default_osr;
  deque<OpSequencer*> op_queue;
  __u64 op_queue_len, op_queue_bytes;
  Cond op_throttle_cond;
  Finisher op_finisher;
  __u64 next_finish;
  map<__u64, pair<Context*,Context*> > finish_queue;

  ThreadPool op_tp;
  struct OpWQ : public ThreadPool::WorkQueue<OpSequencer> {
    FileStore *store;
    OpWQ(FileStore *fs, ThreadPool *tp) : ThreadPool::WorkQueue<OpSequencer>("FileStore::OpWQ", tp), store(fs) {}

    bool _enqueue(OpSequencer *osr) {
      store->op_queue.push_back(osr);
      store->op_queue_len++;
      store->op_queue_bytes += osr->q.back()->bytes;
      return true;
    }
    void _dequeue(OpSequencer *o) {
      assert(0);
    }
    bool _empty() {
      return store->op_queue.empty();
    }
    OpSequencer *_dequeue() {
      if (store->op_queue.empty())
	return NULL;
      OpSequencer *osr = store->op_queue.front();
      store->op_queue.pop_front();
      return osr;
    }
    void _process(OpSequencer *osr) {
      store->_do_op(osr);
    }
    void _process_finish(OpSequencer *osr) {
      store->_finish_op(osr);
    }
    void _clear() {
      assert(store->op_queue.empty());
    }
  } op_wq;

  void _do_op(OpSequencer *o);
  void _finish_op(OpSequencer *o);
  void queue_op(Sequencer *osr, __u64 op, list<Transaction*>& tls, Context *onreadable, Context *onreadable_sync);
  void _journaled_ahead(Sequencer *osr, __u64 op, list<Transaction*> &tls,
			Context *onreadable, Context *ondisk, Context *onreadable_sync);
  friend class C_JournaledAhead;

  // flusher thread
  Cond flusher_cond;
  list<__u64> flusher_queue;
  int flusher_queue_len;
  void flusher_entry();
  struct FlusherThread : public Thread {
    FileStore *fs;
    FlusherThread(FileStore *f) : fs(f) {}
    void *entry() {
      fs->flusher_entry();
      return 0;
    }
  } flusher_thread;
  bool queue_flusher(int fd, __u64 off, __u64 len);

  int open_journal();

 public:
  FileStore(const char *base, const char *jdev = 0) : 
    basedir(base), journalpath(jdev ? jdev:""),
    btrfs(false), btrfs_trans_start_end(false), btrfs_clone_range(false),
    btrfs_snap_create(false),
    btrfs_snap_destroy(false),
    btrfs_snap_create_async(false),
    btrfs_snap_destroy_async(false),
    btrfs_wait_sync(false),
    fsid_fd(-1), op_fd(-1),
    attrs(this), fake_attrs(false), 
    collections(this), fake_collections(false),
    lock("FileStore::lock"),
    sync_epoch(0), stop(false), sync_thread(this),
    op_queue_len(0), op_queue_bytes(0), next_finish(0),
    op_tp("FileStore::op_tp", g_conf.filestore_op_threads), op_wq(this, &op_tp),
    flusher_queue_len(0), flusher_thread(this) {
    // init current_fn
    snprintf(current_fn, sizeof(current_fn), "%s/current", basedir.c_str());
    snprintf(current_op_seq_fn, sizeof(current_op_seq_fn), "%s/current/commit_op_seq", basedir.c_str());
  }

  int _detect_fs();
  int _sanity_check_fs();
  
  bool test_mount_in_use();
  int mount();
  int umount();
  int mkfs();
  int mkjournal();

  int statfs(struct statfs *buf);

  int do_transactions(list<Transaction*> &tls, __u64 op_seq);
  unsigned apply_transaction(Transaction& t, Context *ondisk=0);
  unsigned apply_transactions(list<Transaction*>& tls, Context *ondisk=0);
  int _transaction_start(__u64 bytes, __u64 ops);
  void _transaction_finish(int id);
  unsigned _do_transaction(Transaction& t);

  int queue_transaction(Sequencer *osr, Transaction* t);
  int queue_transactions(Sequencer *osr, list<Transaction*>& tls, Context *onreadable, Context *ondisk=0,
			 Context *onreadable_sync=0);

  // ------------------
  // objects
  int pick_object_revision_lt(sobject_t& oid) {
    return 0;
  }
  bool exists(coll_t cid, const sobject_t& oid);
  int stat(coll_t cid, const sobject_t& oid, struct stat *st);
  int read(coll_t cid, const sobject_t& oid, __u64 offset, size_t len, bufferlist& bl);

  int _touch(coll_t cid, const sobject_t& oid);
  int _write(coll_t cid, const sobject_t& oid, __u64 offset, size_t len, const bufferlist& bl);
  int _zero(coll_t cid, const sobject_t& oid, __u64 offset, size_t len);
  int _truncate(coll_t cid, const sobject_t& oid, __u64 size);
  int _clone(coll_t cid, const sobject_t& oldoid, const sobject_t& newoid);
  int _clone_range(coll_t cid, const sobject_t& oldoid, const sobject_t& newoid, __u64 off, __u64 len);
  int _do_clone_range(int from, int to, __u64 off, __u64 len);
  int _remove(coll_t cid, const sobject_t& oid);

  void _start_sync();

  void sync();
  void sync(Context *onsafe);
  void _flush_op_queue();
  void flush();
  void sync_and_flush();

  // attrs
  int getattr(coll_t cid, const sobject_t& oid, const char *name, void *value, size_t size);
  int getattr(coll_t cid, const sobject_t& oid, const char *name, bufferptr &bp);
  int getattrs(coll_t cid, const sobject_t& oid, map<nstring,bufferptr>& aset, bool user_only = false);

  int _getattr(const char *fn, const char *name, bufferptr& bp);
  int _getattrs(const char *fn, map<nstring,bufferptr>& aset, bool user_only = false);

  int _setattr(coll_t cid, const sobject_t& oid, const char *name, const void *value, size_t size);
  int _setattrs(coll_t cid, const sobject_t& oid, map<nstring,bufferptr>& aset);
  int _rmattr(coll_t cid, const sobject_t& oid, const char *name);
  int _rmattrs(coll_t cid, const sobject_t& oid);

  int collection_getattr(coll_t c, const char *name, void *value, size_t size);
  int collection_getattr(coll_t c, const char *name, bufferlist& bl);
  int collection_getattrs(coll_t cid, map<nstring,bufferptr> &aset);

  int _collection_setattr(coll_t c, const char *name, const void *value, size_t size);
  int _collection_rmattr(coll_t c, const char *name);
  int _collection_setattrs(coll_t cid, map<nstring,bufferptr> &aset);

  // collections
  int list_collections(vector<coll_t>& ls);
  int collection_stat(coll_t c, struct stat *st);
  bool collection_exists(coll_t c);
  bool collection_empty(coll_t c);
  int collection_list_partial(coll_t c, snapid_t seq, vector<sobject_t>& o, int count, collection_list_handle_t *handle);
  int collection_list(coll_t c, vector<sobject_t>& o);

  int _create_collection(coll_t c);
  int _destroy_collection(coll_t c);
  int _collection_add(coll_t c, coll_t ocid, const sobject_t& o);
  int _collection_remove(coll_t c, const sobject_t& o);

  void trim_from_cache(coll_t cid, const sobject_t& oid, __u64 offset, size_t len) {}
  int is_cached(coll_t cid, const sobject_t& oid, __u64 offset, size_t len) { return -1; }
};

#endif
