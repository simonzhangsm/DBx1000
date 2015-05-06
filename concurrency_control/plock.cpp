#include "global.h"
#include "helper.h"
#include "plock.h"
#include "mem_alloc.h"
#include "txn.h"
#include "remote_query.h"

/************************************************/
// per-partition Manager
/************************************************/
void PartMan::init(uint64_t node_id) {
	uint64_t part_id = get_part_id(this);
	_node_id = node_id; 
	waiter_cnt = 0;
	owner = NULL;
	waiters = (txn_man **)
		mem_allocator.alloc(sizeof(txn_man *) * g_node_cnt * MAX_TXN_IN_FLIGHT, part_id);
	pthread_mutex_init( &latch, NULL );
}

void PartMan::start_spec_ex() {
  pthread_mutex_lock( &latch );

  base_query * owner_qry = txn_pool.get_qry(GET_NODE_ID(owner->get_pid()), owner->get_txn_id());
  for (UInt32 i = 0; i < waiter_cnt - 1; i++) {
    if(waiters[i]->spec)
      continue;
    if(waiters[i]->parts_locked > 1) 
      continue;
      //check for conflicts
    // TODO: Check for conflicts with all waiters earlier in queue, not just the plock owner
    base_query * waiter_qry = txn_pool.get_qry(GET_NODE_ID(waiters[i]->get_pid()), waiters[i]->get_txn_id());
    if(waiters[i]->conflict(owner_qry,waiter_qry))
      continue;
    waiters[i]->spec = true;
    txn_pool.restart_txn(waiters[i]->get_txn_id());
  }
  

  pthread_mutex_unlock( &latch );
}

RC PartMan::lock(txn_man * txn) {
  RC rc;

  pthread_mutex_lock( &latch );
  if (owner == NULL) {
    owner = txn;
		// If not local, send remote response
		if(GET_NODE_ID(owner->get_pid()) != _node_id)
			remote_rsp(true,RCOK,owner);
    rc = RCOK;
  } else if (owner->get_ts() < txn->get_ts()) {
    int i;
    // depends on txn in flight
		//assert(waiter_cnt < (g_thread_cnt * g_node_cnt -1 ));
    for (i = waiter_cnt; i > 0; i--) {
      if (txn->get_ts() > waiters[i - 1]->get_ts()) {
        waiters[i] = txn;
        break;
      } else 
        waiters[i] = waiters[i - 1];
    }
    if (i == 0)
      waiters[i] = txn;
    waiter_cnt ++;
		if(GET_NODE_ID(txn->get_pid()) == _node_id)
      ATOM_ADD(txn->ready_part, 1);
    rc = WAIT;
    txn->rc = rc;
    txn->wait_starttime = get_sys_clock();
  } else {
    rc = Abort;
		// if we abort, need to send abort to remote node
		if(GET_NODE_ID(txn->get_pid()) != _node_id)
			remote_rsp(true,rc,txn);
  }
  pthread_mutex_unlock( &latch );
  return rc;
}

void PartMan::unlock(txn_man * txn) {
  pthread_mutex_lock( &latch );
  if (txn == owner) {   
    if (waiter_cnt == 0) {
      owner = NULL;
    }
    else {
      owner = waiters[0];     
      // TODO: Calculate plock wait time here
      for (UInt32 i = 0; i < waiter_cnt - 1; i++) {
        assert( waiters[i]->get_ts() < waiters[i + 1]->get_ts() );
        waiters[i] = waiters[i + 1];
      }
      waiter_cnt --;
      // FIXME: stat locality w/ thd_id
      INC_STATS(0,time_wait_lock,get_sys_clock() - owner->wait_starttime);
			if(GET_NODE_ID(owner->get_pid()) == _node_id) {
        ATOM_SUB(owner->ready_part, 1);
        // If local and ready_part is 0, restart txn
#if !SPEC_EX
        if(owner->ready_part == 0 && owner->get_rsp_cnt() == 0) {
          owner->state = EXEC; 
          txn_pool.restart_txn(owner->get_txn_id());
        }
#endif
      }
      else {
        // FIXME: stat locality w/ thd_id
        INC_STATS(0,rtime_wait_lock,get_sys_clock() - owner->wait_starttime);
				remote_rsp(true,RCOK,owner);
			}
    } 
  } else {
    bool find = false;
    for (UInt32 i = 0; i < waiter_cnt; i++) {
      if (waiters[i] == txn) 
        find = true;
      if (find && i < waiter_cnt - 1) 
        waiters[i] = waiters[i + 1];
    }
    /*
		if(GET_NODE_ID(txn->get_pid()) == _node_id)
      ATOM_SUB(txn->ready_ulk, 1);
      */
		// We may not find a remote request among the waiters; ignore
    //assert(find);
		if(find)
      waiter_cnt --;
  }
	// Send response for rulk
  /*
  if(GET_NODE_ID(txn->get_pid()) != _node_id)
	  remote_rsp(false,RCOK,txn);
    */
  // If local, decr ready_ulk
	if(GET_NODE_ID(txn->get_pid()) == _node_id)
    ATOM_SUB(txn->ready_ulk, 1);

  pthread_mutex_unlock( &latch );
}


void PartMan::remote_rsp(bool l, RC rc, txn_man * txn) {
  rem_qry_man.ack_response(rc,txn);
  /*
  uint64_t pid = txn->get_pid();
  uint64_t node_id = GET_NODE_ID(pid);
  uint64_t ts = txn->get_ts();
  
	int max_num = 6;
	void ** data = new void *[max_num];
	int * sizes = new int [max_num];
	int num = 0;
	uint64_t _pid = pid;
	uint64_t _ts = ts;
	RC _rc = rc;
	RemReqType rtype = l ? RLK_RSP : RULK_RSP;
  txnid_t tid = txn->get_txn_id(); 

	data[num] = &tid;
	sizes[num++] = sizeof(txnid_t);

	data[num] = &rtype;
	sizes[num++] = sizeof(RemReqType);
	data[num] = &_rc;
	sizes[num++] = sizeof(RC);
	data[num] = &_pid;
	sizes[num++] = sizeof(uint64_t);
	data[num] = &_ts;
	sizes[num++] = sizeof(uint64_t);

	rem_qry_man.send_remote_rsp(node_id,data,sizes,num);
  */
}
/************************************************/
// Partition Lock
/************************************************/

void Plock::init(uint64_t node_id) {
	_node_id = node_id;
	ARR_PTR(PartMan, part_mans, g_part_cnt);
	for (UInt32 i = 0; i < g_part_cnt; i++)
		part_mans[i]->init(node_id);
}

void Plock::start_spec_ex(uint64_t * parts, uint64_t part_cnt) {
	for (uint64_t i = 0; i < part_cnt; i ++) {
		uint64_t part_id = parts[i];
		if(GET_NODE_ID(part_id) == get_node_id())  {
      part_mans[part_id]->start_spec_ex();
    }
  }
}

RC Plock::lock(uint64_t * parts, uint64_t part_cnt, txn_man * txn) {
	uint64_t tid = txn->get_thd_id();
  // Part ID is at home node
	uint64_t nid = txn->get_node_id();
  txn->set_pid(GET_PART_ID(tid,nid));
  txn->parts_locked = 0;
	RC rc = RCOK;
  txn->rc = RCOK;
	ts_t starttime = get_sys_clock();
	UInt32 i;
	for (i = 0; i < part_cnt; i ++) {
		uint64_t part_id = parts[i];
    txn->parts_locked++;
		if(GET_NODE_ID(part_id) == get_node_id())  {
			rc = part_mans[part_id]->lock(txn);
		}
		else {
			// Increment txn->ready_part; Pass txn to remote thr somehow?
			// Have some Plock shared object and spin on that instead of txn object?
			ATOM_ADD(txn->ready_part,1);
			remote_qry(true, part_id, txn);
		}
		if (rc == Abort || txn->rc == Abort)
			break;
	}
	if (txn->ready_part > 0 && !(rc == Abort || txn->rc == Abort)) {
    rc = WAIT;
    txn->rc = rc;
    txn->wait_starttime = get_sys_clock();
    return rc;

	}
	// Abort and send unlock requests as necessary
	if (rc == Abort || txn->rc == Abort) {
    rc = Abort;
    txn->rc = rc;
    return rc;

   }
	assert(txn->ready_part == 0);
	INC_STATS(tid, time_lock_man, get_sys_clock() - starttime);
	return RCOK;
}

RC Plock::unlock(uint64_t * parts, uint64_t part_cnt, txn_man * txn) {
	uint64_t tid = txn->get_thd_id();
	//uint64_t nid = txn->get_node_id();
	ts_t starttime = get_sys_clock();
  // TODO: Store # parts locked or sent locks in qry
  // TODO: Only send to each node once, regardless of # of parts 
	//for (UInt32 i = 0; i < part_cnt; i ++) {
	for (UInt32 i = 0; i < txn->parts_locked; i ++) {
		uint64_t part_id = parts[i];
		if(GET_NODE_ID(part_id) == get_node_id()) {
      ATOM_ADD(txn->ready_ulk,1);
			part_mans[part_id]->unlock(txn);
    }
		else {
      ATOM_ADD(txn->ready_ulk,1);
			remote_qry(false,part_id,txn);
    }
	}
  if(txn->ready_ulk > 0) {
    txn->wait_starttime = get_sys_clock();
    txn->rc = WAIT;
    return WAIT;

  }
	assert(txn->ready_ulk == 0);
	INC_STATS(tid, time_lock_man, get_sys_clock() - starttime);
  return RCOK;
}

void Plock::unpack_rsp(base_query * query, void * d) {
	char * data = (char *) d;
	uint64_t ptr = HEADER_SIZE + sizeof(txnid_t) + sizeof(RemReqType);
	memcpy(&query->rc,&data[ptr],sizeof(RC));
	ptr += sizeof(query->rc);
	memcpy(&query->pid,&data[ptr],sizeof(query->pid));
	ptr += sizeof(query->pid);
	memcpy(&query->ts,&data[ptr],sizeof(query->ts));
	ptr += sizeof(query->ts);
}

void Plock::unpack(base_query * query, char * data) {
	uint64_t ptr = HEADER_SIZE + sizeof(txnid_t) + sizeof(RemReqType);
	assert(query->rtype == RLK || query->rtype == RULK);
		
	memcpy(&query->pid,&data[ptr],sizeof(query->pid));
	ptr += sizeof(query->pid);
	memcpy(&query->ts,&data[ptr],sizeof(query->ts));
	ptr += sizeof(query->ts);
	memcpy(&query->part_cnt,&data[ptr],sizeof(query->part_cnt));
	ptr += sizeof(query->part_cnt);
	query->parts = new uint64_t[query->part_cnt];
	for (uint64_t i = 0; i < query->part_cnt; i++) {
		memcpy(&query->parts[i],&data[ptr],sizeof(query->parts[i]));
		ptr += sizeof(query->parts[i]);
	}
}

void Plock::remote_qry(bool l, uint64_t lid, txn_man * txn) {
	assert(GET_NODE_ID(lid) != _node_id);
	int num = 0;
	int max_num = 6;
	uint64_t part_cnt = 1;
	uint64_t _ts = txn->get_ts();
	uint64_t _pid = txn->get_pid();
	uint64_t _lid = lid;
	RemReqType rtype = l ? RLK : RULK;
	void ** data = new void *[max_num];
	int * sizes = new int [max_num];

  txnid_t tid = txn->get_txn_id(); 

	data[num] = &tid;
	sizes[num++] = sizeof(txnid_t);

	data[num] = &rtype;
	sizes[num++] = sizeof(RemReqType);
	data[num] = &_pid;
	sizes[num++] = sizeof(uint64_t);
	data[num] = &_ts;
	sizes[num++] = sizeof(uint64_t);
	data[num] = &part_cnt;
	sizes[num++] = sizeof(uint64_t);
	data[num] = &_lid;
	sizes[num++] = sizeof(uint64_t);

	rem_qry_man.send_remote_query(GET_NODE_ID(lid),data,sizes,num);
}

RC Plock::rem_lock(uint64_t * parts, uint64_t part_cnt, txn_man * txn) {
  RC rc;
  assert(part_cnt >= 1);
	ts_t starttime = get_sys_clock();
	for (UInt32 i = 0; i < part_cnt; i ++) {
		uint64_t part_id = parts[i];
		assert(GET_NODE_ID(part_id) == get_node_id());
		rc = part_mans[part_id]->lock(txn);
	}
	INC_STATS(1, rtime_lock_man, get_sys_clock() - starttime);
  return rc;
}

void Plock::rem_unlock(uint64_t * parts, uint64_t part_cnt, txn_man * txn) {
	ts_t starttime = get_sys_clock();
	for (UInt32 i = 0; i < part_cnt; i ++) {
		uint64_t part_id = parts[i];
		assert(GET_NODE_ID(part_id) == get_node_id());
		part_mans[part_id]->unlock(txn);
	}
	INC_STATS(1, rtime_lock_man, get_sys_clock() - starttime);
}

void Plock::rem_lock_rsp(RC rc, txn_man * txn) {
	ts_t starttime = get_sys_clock();
	if(rc != RCOK) {
    assert(rc == Abort);
    txn->rc = rc;
  }
	ATOM_SUB(txn->ready_part, 1);
	INC_STATS(1, time_lock_man, get_sys_clock() - starttime);
}

void Plock::rem_unlock_rsp(txn_man * txn) {
	ts_t starttime = get_sys_clock();
	ATOM_SUB(txn->ready_ulk, 1);
	INC_STATS(1, time_lock_man, get_sys_clock() - starttime);
}
