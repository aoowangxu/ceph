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

#ifndef PRIORITY_QUEUE_H
#define PRIORITY_QUEUE_H

#include "common/Mutex.h"
#include "common/Formatter.h"
#include "common/ceph_context.h"
#include "common/config.h"

#include <map>
#include <utility>
#include <list>
#include <algorithm>
#include <time.h>
#include <cstdlib>
#include <sstream>

/**
 * Manages queue for normal and strict priority items
 *
 * On dequeue, the queue will select the lowest priority queue
 * such that the q has bucket > cost of front queue item.
 *
 * If there is no such queue, we choose the next queue item for
 * the highest priority queue.
 *
 * Before returning a dequeued item, we place into each bucket
 * cost * (priority/total_priority) tokens.
 *
 * enqueue_strict and enqueue_strict_front queue items into queues
 * which are serviced in strict priority order before items queued
 * with enqueue and enqueue_front
 *
 * Within a priority class, we schedule round robin based on the class
 * of type K used to enqueue items.  e.g. you could use entity_inst_t
 * to provide fairness for different clients.
 */

//#pragma push_macro("dout_subsys")
//#undef dout_subsys
//#define dout_subsys ceph_subsys_osd
//
//#pragma push_macro("dout_prefix")
//#undef dout_prefix
//#define dout_prefix  *_dout << "dm_queue: "

struct SLO {
  unsigned reserve;
  unsigned prop;
  unsigned limit;
  SLO() :
    reserve(0), prop(0), limit(0) {
    }

  SLO(unsigned r, unsigned p, unsigned l) {
    reserve = r;
    prop = p;
    limit = l;
  }
};

class CephContext;

template<typename T, typename K>
class PrioritizedQueue {
  int64_t total_priority;
  int64_t max_tokens_per_subqueue;
  int64_t min_cost;
  CephContext *cct;
  std::string status;

  // data structure for dmClock
  enum tag_types_t {
  	Q_NONE = -1, Q_RESERVE = 0, Q_PROP, Q_LIMIT, Q_COUNT
  };
  typedef std::list<std::pair<double, T> > ListPairs; //will hold deadline time-stamp
  template<class F>
    static unsigned filter_list_pairs(ListPairs *l, F f, std::list<T> *out) {
      unsigned ret = 0;
      if (out) {
	for (typename ListPairs::reverse_iterator i = l->rbegin();
	    i != l->rend(); ++i) {
	  if (f(i->second)) {
	    out->push_front(i->second);
	  }
	}
      }
      for (typename ListPairs::iterator i = l->begin(); i != l->end();) {
	if (f(i->second)) {
	  l->erase(i++);
	  ++ret;
	} else {
	  ++i;
	}
      }
      return ret;
    }

  struct SubQueue {
    private:
      typedef std::map<K, ListPairs> Classes;
      Classes q;
      unsigned tokens, max_tokens;
      int64_t size;
      typename Classes::iterator cur;
    public:
      SubQueue(const SubQueue &other) :
	q(other.q), tokens(other.tokens), max_tokens(other.max_tokens), size(
	    other.size), cur(q.begin()) {
	}
      SubQueue() :
	tokens(0), max_tokens(0), size(0), cur(q.begin()) {
	}
      void set_max_tokens(unsigned mt) {
	max_tokens = mt;
      }
      unsigned get_max_tokens() const {
	return max_tokens;
      }
      unsigned num_tokens() const {
	return tokens;
      }
      void put_tokens(unsigned t) {
	tokens += t;
	if (tokens > max_tokens)
	  tokens = max_tokens;
      }
      void take_tokens(unsigned t) {
	if (tokens > t)
	  tokens -= t;
	else
	  tokens = 0;
      }
      void enqueue(K cl, unsigned cost, T item) {
	q[cl].push_back(std::make_pair(cost, item));
	if (cur == q.end())
	  cur = q.begin();
	size++;
      }
      void enqueue_front(K cl, unsigned cost, T item) {
	q[cl].push_front(std::make_pair(cost, item));
	if (cur == q.end())
	  cur = q.begin();
	size++;
      }
      std::pair<unsigned, T> front() const {
	assert(!(q.empty()));
	assert(cur != q.end());
	return cur->second.front();
      }
      void pop_front() {
	assert(!(q.empty()));
	assert(cur != q.end());
	cur->second.pop_front();
	if (cur->second.empty())
	  q.erase(cur++);
	else
	  ++cur;
	if (cur == q.end())
	  cur = q.begin();
	size--;
      }
      unsigned length() const {
	assert(size >= 0);
	return (unsigned) size;
      }
      bool empty() const {
	return q.empty();
      }
      template<class F>
	void remove_by_filter(F f, std::list<T> *out) {
	  for (typename Classes::iterator i = q.begin(); i != q.end();) {
	    size -= filter_list_pairs(&(i->second), f, out);
	    if (i->second.empty()) {
	      if (cur == i)
		++cur;
	      q.erase(i++);
	    } else {
	      ++i;
	    }
	  }
	  if (cur == q.end())
	    cur = q.begin();
	}
      void remove_by_class(K k, std::list<T> *out) {
	typename Classes::iterator i = q.find(k);
	if (i == q.end())
	  return;
	size -= i->second.size();
	if (i == cur)
	  ++cur;
	if (out) {
	  for (typename ListPairs::reverse_iterator j =
	      i->second.rbegin(); j != i->second.rend(); ++j) {
	    out->push_front(j->second);
	  }
	}
	q.erase(i);
	if (cur == q.end())
	  cur = q.begin();
      }

      void dump(Formatter *f) const {
	f->dump_int("tokens", tokens);
	f->dump_int("max_tokens", max_tokens);
	f->dump_int("size", size);
	f->dump_int("num_keys", q.size());
	if (!empty())
	  f->dump_int("first_item_cost", front().first);
      }
  };

  struct SubQueueDMClock {
    private:
      typedef std::map<K, ListPairs > Requests;
      Requests requests;
      unsigned throughput_available, throughput_prop, throughput_system;
      int64_t size;
      int64_t virtual_clock;
      CephContext * cct;



      struct Tag {
	double_t r_deadline, r_spacing;
	double_t p_deadline, p_spacing;
	double_t l_deadline, l_spacing;
	bool active;
	tag_types_t selected_tag;
	K cl;
	SLO slo;
	double_t stat;
	int64_t last_delta, last_rho;

	Tag(K _cl, SLO _slo) :
	  r_deadline(0), r_spacing(0), p_deadline(0), p_spacing(0), l_deadline(
	      0), l_spacing(0), active(true), selected_tag(
		Q_NONE), cl(_cl), slo(_slo), stat(0), last_delta(0), last_rho(0) {
	      }
	Tag(utime_t t) :
	  r_deadline(t), r_spacing(0), p_deadline(t), p_spacing(0), l_deadline(
	      t), l_spacing(0), active(true), selected_tag(
		Q_NONE), stat(0), last_delta(0), last_rho(0) {
	      }

	Tag(int64_t t) :
	  r_deadline(t), r_spacing(0), p_deadline(t), p_spacing(0), l_deadline(
	      t), l_spacing(0), active(true), selected_tag(
		Q_NONE), stat(0), last_delta(0), last_rho(0) {
	      }
      };
      typedef std::vector<Tag> Schedule;
      Schedule schedule;

      struct Deadline {
	size_t cl_index;
	double_t deadline;
	bool valid;
	Deadline() :
	  cl_index(0), deadline(0), valid(false) {
	  }
	void set_values(size_t ci, double_t d, bool v = true) {
	  cl_index = ci;
	  deadline = d;
	  valid = v;
	}
      };
      Deadline min_tag_r, min_tag_p;

      void create_new_tag(K cl, SLO slo) {
	Tag tag(cl, slo);
	if (slo.reserve) {
	  tag.r_deadline = get_current_clock();
	  tag.r_spacing = (double_t) get_system_throughput()
	    / slo.reserve;
	  reserve_throughput(slo.reserve);
	}
	if (slo.limit) {
	  assert(slo.limit > slo.reserve);
	  tag.l_deadline = get_current_clock();
	  tag.l_spacing = (double_t) get_system_throughput() / slo.limit;
	}

	if (slo.prop) {
	  reserve_prop_throughput(slo.prop);
	  double_t prop = calculate_prop_throughput(slo.prop);
	  assert(prop > 0);
	  tag.p_spacing = (double_t) get_system_throughput() / prop;
	  tag.p_deadline =
	    min_tag_p.deadline ?
	    min_tag_p.deadline : get_current_clock();

	  recalculate_prop_throughput();
	}
	schedule.push_back(tag);
	update_min_deadlines();
      }

      void update_active_tag(size_t cl_index) {
	Tag *tag = &schedule[cl_index];

	if (tag->selected_tag == Q_RESERVE) {
	  if (tag->r_deadline)
	    tag->r_deadline = tag->r_deadline + tag->r_spacing;
	}
	if (tag->p_deadline) {
	  tag->p_deadline = tag->p_deadline + tag->p_spacing;
	}
	if (tag->l_deadline) {
	  tag->l_deadline = tag->l_deadline + tag->l_spacing;
	}
	update_min_deadlines();
      }

      // a separate function to update idle tags
      // for better performance.
      void update_idle_tag(size_t cl_index) {
	int64_t now = get_current_clock();
	Tag *tag = &schedule[cl_index];
	tag->active = true;

	if (tag->r_deadline) {
	  tag->r_deadline = std::max((tag->r_deadline + tag->r_spacing),
	      (double_t) now);
	}
	if (tag->p_deadline) {
	  tag->p_deadline = min_tag_p.deadline ? min_tag_p.deadline : now;
	}
	if (tag->l_deadline) {
	  tag->l_deadline = std::max((tag->l_deadline + tag->l_spacing),
	      (double_t) now);
	}
	update_min_deadlines();
      }

      void update_min_deadlines() {
	min_tag_r.valid = min_tag_p.valid = false;
	size_t index = 0;
	for (typename Schedule::iterator it = schedule.begin();
	    it != schedule.end(); ++it, index++) {
	  Tag tag = *it;
	  if (!tag.active)
	    continue;

	  if (tag.r_deadline
	      && ((tag.r_deadline >= tag.l_deadline)
		|| (tag.l_deadline <= get_current_clock()))) {
	    if (min_tag_r.valid) {
	      if (min_tag_r.deadline >= tag.r_deadline)
		min_tag_r.set_values(index, tag.r_deadline);
	    } else {
	      min_tag_r.set_values(index, tag.r_deadline);
	    }
	  }

	  if (tag.p_deadline && (tag.l_deadline <= get_current_clock())) {
	    if (min_tag_p.valid) {
	      if (min_tag_p.deadline >= tag.p_deadline)
		min_tag_p.set_values(index, tag.p_deadline);
	    } else {
	      min_tag_p.set_values(index, tag.p_deadline);
	    }
	  }
	}
      }

      void issue_idle_cycle() {
	//ldout(cct, 0) << get_current_clock() << "____idle_____" << "\t" << dendl;
	//print_current_tag(Q_NONE);
//	if(cct)
//	  ldout(cct, 0) << "idle cycle issued "<< dendl;
	increment_clock();
	update_min_deadlines();
      }

      double_t calculate_prop_throughput(double_t prop) const {
	if (throughput_prop && prop) {
	  if (prop <= throughput_prop)
	    return throughput_available * (prop / throughput_prop);
	  else
	    return throughput_available;
	}
	return 0;
      }

      void recalculate_prop_throughput() {
	double_t prop;
	for (typename Schedule::iterator it = schedule.begin();
	    it != schedule.end(); ++it) {
	  if (it->slo.prop) {
	    prop = calculate_prop_throughput(it->slo.prop);
	    assert(prop > 0);
	    it->p_spacing = (double_t) get_system_throughput() / prop;
	  }
	}
      }

      bool get_client_index(K cl, size_t &index) {
	bool is_found = false;
	for (typename Schedule::iterator it = schedule.begin();
	    it != schedule.end(); ++it) {
	  if (it->cl == cl) {
	    index = it - schedule.begin();
	    is_found = true;
	    break;
	  }
	}
	return is_found;
      }

      //helper function
      std::string print_iops() {
	std::ostringstream data;
	data << "throughput at " << get_current_clock() << ":" << std::endl;
	for (size_t i = 0; i < schedule.size(); i++)
	  data << "\t cl " << schedule[i].cl << " IOPS :" << schedule[i].stat << std::endl;
	return data.str();
      }

      // helper function
      std::string print_current_tag(tag_types_t tt, int index = -1) {
	std::ostringstream data;
	data << "current time: "<< get_current_clock() << " deadlines:"<<std::endl;
	for (typename Schedule::iterator it = schedule.begin();
	    it != schedule.end(); ++it) {
	  Tag _tag = *it;
	  if (index == (it - schedule.begin())) {
	    if (tt == Q_RESERVE)
	      data << "*";
	    if (tt == Q_PROP)
	      data << "~";
	    if (tt == Q_LIMIT)
	      data << "_";
	  }
	  data <<"\t["<< _tag.r_deadline << " , " << _tag.p_deadline << " , "
	    << _tag.l_deadline << " ]" << std::endl;
	}
	return data.str();
      }

    public:
      SubQueueDMClock(const SubQueueDMClock &other) :
	requests(other.requests), throughput_available(
	    other.throughput_available), throughput_prop(
	      other.throughput_prop), throughput_system(
		other.throughput_system), size(other.size), schedule(
		  other.schedule), virtual_clock(other.virtual_clock),
		  cct(other.cct){
		}

      SubQueueDMClock(CephContext* cct_= NULL) :
	throughput_available(0), throughput_prop(0), throughput_system(
	    0), size(0), virtual_clock(1), cct(cct_) {
	}

      int64_t get_current_clock() const{
	//should call ceph_time_now()
	return virtual_clock;
      }

      int64_t increment_clock() {
	//should issue wait cycle
	if ((virtual_clock % throughput_system) == 0) {
	  print_iops();
	}
	return ++virtual_clock;
      }

      void set_system_throughput(unsigned mt) {
	throughput_system = mt;
	throughput_available = mt;
      }

      unsigned get_system_throughput() const {
	return throughput_system;
      }

      unsigned get_available_throughput() const {
	return throughput_available;
      }

      void release_throughput(unsigned t) {
	throughput_available += t;
	if (throughput_available > throughput_system)
	  throughput_available = throughput_system;
      }

      void reserve_throughput(unsigned t) {
	if (throughput_available > t)
	  throughput_available -= t;
	else
	  throughput_available = 0;
      }

      void release_prop_throughput(unsigned t) {
	throughput_prop -= t;
	if (throughput_prop < 0)
	  throughput_prop = 0;
      }

      void reserve_prop_throughput(unsigned t) {
	throughput_prop += t;
      }

      void purge_idle_clients() {
	bool update_required = false;
	typename Schedule::iterator it = schedule.begin();
	for (; it != schedule.end();) {
	  if (!it->active) {
	    update_required = true;
	    if (it->slo.reserve)
	      release_throughput(it->slo.reserve);
	    if (it->slo.prop)
	      release_prop_throughput(it->slo.prop);

	    //print_iops(); //testing

	    requests.erase(it->cl);
	    it = schedule.erase(it);
	  } else {
	    ++it;
	  }
	}
	if (update_required)
	  recalculate_prop_throughput();
      }

      template<class F>
	void remove_by_filter(F f, std::list<T> *out) {
	  bool purge_required = false;
	  for (typename Requests::iterator i = requests.begin(); i != requests.end(); ++i) {
	    size -= filter_list_pairs(&(i->second), f, out);
	    if (i->second.empty()) {
	      size_t index;
	      if(get_client_index( i->first, index)){
		schedule[index].active = false;
		purge_required = true;
	      }
	    }
	  }
	  if(purge_required)
	    purge_idle_clients();

//	  if(cct)
//	    ldout(cct,0) <<__func__ <<" purge idle clients" <<dendl;
	}

      void remove_by_class(K k, std::list<T> *out) {
	typename Requests::iterator i = requests.find(k);
	if (i == requests.end())
	  return;
	size -= i->second.size();
	if (out) {
	  for (typename ListPairs::reverse_iterator j =
	      i->second.rbegin(); j != i->second.rend(); ++j) {
	    out->push_front(j->second);
	  }
	}
	purge_idle_clients();
      }

      Tag* front(size_t &out) {
	assert((size != 0));
	int64_t t = get_current_clock();

	if (min_tag_r.valid) {
	  Tag *tag = &schedule[min_tag_r.cl_index];
	  if (tag->r_deadline <= t) {
	    tag->selected_tag = Q_RESERVE;
	    out = min_tag_r.cl_index;
	    return tag;
	  }
	}
	if (min_tag_p.valid) {
	  Tag *tag = &schedule[min_tag_p.cl_index];
	  if (tag->p_deadline) {
	    tag->selected_tag = Q_PROP;
	    out = min_tag_p.cl_index;
	    return tag;
	  }
	}
	return NULL;
      }

      std::pair<T, tag_types_t> pop_front() {
	assert((size != 0));
	size_t cl_index;
	Tag *tag = front(cl_index);

	// issue idle cycle
	while (size && tag == NULL) {
	  issue_idle_cycle();
	  tag = front(cl_index);
	}
	//debug
	//print_current_tag(tag->selected_tag, cl_index);
	tag->stat++;

	T ret = requests[tag->cl].front().second;
	requests[tag->cl].pop_front();
	if (requests[tag->cl].empty())
	  tag->active = false;

//	if(cct){
//	    ldout(cct, 0) << __func__<< " ID " << tag->cl << " size: "<< size <<" #client "<< schedule.size() <<dendl;
//	}
	increment_clock();
	update_active_tag(cl_index);
	size--;
	return std::make_pair(ret, tag->selected_tag);
      }

      void enqueue(K cl, SLO slo, double cost, T item) {
	bool new_cl = (requests.find(cl) == requests.end());
	if (new_cl) {
	  create_new_tag(cl, slo);
//	  if(cct)
//	    ldout(cct, 0) << "new client "<< cl << dendl;
	} else {
	  if (requests[cl].empty()) {
	    size_t index = 0;
	    bool found = get_client_index(cl, index);
	    assert(found != false);
	    update_idle_tag(index);
//	    if(cct)
//	      ldout(cct, 0) << "was idle client "<< cl << dendl;
	  }
	}
	requests[cl].push_back(std::make_pair(cost, item));
	size++;
//	if(cct)
//	  ldout(cct, 0) << __func__ <<" num_msg: "<< size << "map-size "<<requests.size() << " num_schedule: "<<schedule.size() << dendl;
      }

      void enqueue_front(K cl, SLO slo, double cost, T item) {
	bool new_cl = (requests.find(cl) == requests.end());
	if (new_cl) {
	  create_new_tag(cl, slo);
	} else {
	  if (requests[cl].empty()) {
	    size_t index = 0;
	    bool found = get_client_index(cl, index);
	    assert(found != false);
	    update_idle_tag(index);
	  }
	}
	requests[cl].push_front(std::make_pair(cost, item));
	size++;
      }


      unsigned length() const {
	assert(size >= 0);
	return (unsigned) size;
      }

      bool empty() const {
	return (size == 0);
      }

      void dump(Formatter *f) const {
	f->dump_int("system_throughput", throughput_system);
	f->dump_int("available throughput", throughput_available);
	f->dump_int("num_req", size);
	f->dump_int("num_client", schedule.size());
	f->dump_int("num_map", requests.size());
	f->dump_int("cur_clock", get_current_clock());

	f->open_array_section("dm_queue");
	for (size_t i = 0; i < schedule.size(); i++){
	    f->open_object_section("client_iops");
	    f->open_object_section("client");
	    schedule[i].cl.dump(f);
	    f->close_section(); //client
	    f->dump_int("iops" , schedule[i].stat);
	    std::ostringstream data;
	    data <<"["<< schedule[i].r_deadline << " , " << schedule[i].p_deadline << " , "
	    	    << schedule[i].l_deadline << " ]";
	    f->dump_string("deadline",  data.str());
	    f->close_section(); //client_iops
	}
	f->close_section(); //dm_queue
      }

      std::string print_status() {
	std::ostringstream data;
	data << "sys_throughput: " << throughput_system << std::endl;
	data << "avail throughput: " << throughput_available << std::endl;
	data << "num_req: " << size << std::endl;
	data << "num_client: " << schedule.size() << std::endl;
	data << "num_map: " << requests.size() << std::endl;
	data << "clients: \n";
	for(size_t i= 0 ; i <schedule.size(); i++ )
	  data<<"\t"<<schedule[i].cl<<std::endl;

	//if (!empty()) {
	  data << print_iops() << std::endl;
	  data << print_current_tag(Q_NONE) << std::endl;
	//}
	return data.str();
      }
  };

  typedef std::map<unsigned, SubQueue> SubQueues;
  SubQueues high_queue;
  SubQueues queue;
  SubQueueDMClock dm_queue;

  SubQueue *create_queue(unsigned priority) {
    typename SubQueues::iterator p = queue.find(priority);
    if (p != queue.end())
      return &p->second;
    total_priority += priority;
    SubQueue *sq = &queue[priority];
    sq->set_max_tokens(max_tokens_per_subqueue);
    return sq;
  }

  void remove_queue(unsigned priority) {
    assert(queue.count(priority));
    queue.erase(priority);
    total_priority -= priority;
    assert(total_priority >= 0);
  }

  void distribute_tokens(unsigned cost) {
    if (total_priority == 0)
      return;
    for (typename SubQueues::iterator i = queue.begin(); i != queue.end();
	++i) {
      i->second.put_tokens(((i->first * cost) / total_priority) + 1);
    }
  }

  public:
  PrioritizedQueue(unsigned max_per, unsigned min_c, CephContext *cct_ = NULL) :
    total_priority(0), max_tokens_per_subqueue(max_per), min_cost(min_c), cct(cct_), dm_queue(cct_) {

      //dmclock set system throughput to 1500 iops
      //dm_queue.set_system_throughput(max_tokens_per_subqueue);

      dm_queue.set_system_throughput(1500);
    }

  unsigned length() const {
    unsigned total = 0;
    for (typename SubQueues::const_iterator i = queue.begin();
	i != queue.end(); ++i) {
      assert(i->second.length());
      total += i->second.length();
    }
    for (typename SubQueues::const_iterator i = high_queue.begin();
	i != high_queue.end(); ++i) {
      assert(i->second.length());
      total += i->second.length();
    }
    total += dm_queue.length();
    return total;
  }

  template<class F>
    void remove_by_filter(F f, std::list<T> *removed = 0) {
      for (typename SubQueues::iterator i = queue.begin(); i != queue.end();
	  ) {
	unsigned priority = i->first;

	i->second.remove_by_filter(f, removed);
	if (i->second.empty()) {
	  ++i;
	  remove_queue(priority);
	} else {
	  ++i;
	}
      }
      for (typename SubQueues::iterator i = high_queue.begin();
	  i != high_queue.end();) {
	i->second.remove_by_filter(f, removed);
	if (i->second.empty()) {
	  high_queue.erase(i++);
	} else {
	  ++i;
	}
      }
    }

  template<class F>
    void remove_by_filter_dmClock(F f, std::list<T> *removed = 0) {
      dm_queue.remove_by_filter(f, removed);

      for (typename SubQueues::iterator i = high_queue.begin();
	  i != high_queue.end();) {
	i->second.remove_by_filter(f, removed);
	if (i->second.empty()) {
	  high_queue.erase(i++);
	} else {
	  ++i;
	}
      }
    }

  void remove_by_class(K k, std::list<T> *out = 0) {
    for (typename SubQueues::iterator i = queue.begin(); i != queue.end();
	) {
      i->second.remove_by_class(k, out);
      if (i->second.empty()) {
	unsigned priority = i->first;
	++i;
	remove_queue(priority);
      } else {
	++i;
      }
    }
    for (typename SubQueues::iterator i = high_queue.begin();
	i != high_queue.end();) {
      i->second.remove_by_class(k, out);
      if (i->second.empty()) {
	high_queue.erase(i++);
      } else {
	++i;
      }
    }
  }

  void remove_by_class_dmClock(K k, std::list<T> *out = 0) {
    dm_queue.remove_by_class(k, out);

    for (typename SubQueues::iterator i = high_queue.begin();
	i != high_queue.end();) {
      i->second.remove_by_class(k, out);
      if (i->second.empty()) {
	high_queue.erase(i++);
      } else {
	++i;
      }
    }
  }

  void enqueue_strict(K cl, unsigned priority, T item) {
    high_queue[priority].enqueue(cl, 0, item);
  }

  void enqueue_strict_front(K cl, unsigned priority, T item) {
    high_queue[priority].enqueue_front(cl, 0, item);
  }

  void enqueue_dmClock(K cl, unsigned slo_reserve, unsigned slo_prop,
      unsigned slo_limit, unsigned cost,  T item) { //int64_t delta, int64_t rho,
//    if(cct){
//	ldout(cct, 0) << __func__<< "client_id: "<< cl << " cost " << cost << " reserve " << slo_reserve
//        	  << " prop " << slo_prop << " limit " << slo_limit << dendl;
//    }
    dm_queue.enqueue(cl, SLO(slo_reserve, slo_prop, slo_limit), cost, item);
  }

  void enqueue_front_dmClock(K cl, unsigned slo_reserve, unsigned slo_prop,
      unsigned slo_limit, unsigned cost, T item) { //, int64_t delta, int64_t rho
    dm_queue.enqueue(cl, SLO(slo_reserve, slo_prop, slo_limit), cost, item);
  }

  void enqueue(K cl, unsigned priority, unsigned cost, T item) {
    if (cost < min_cost)
      cost = min_cost;
    if (cost > max_tokens_per_subqueue)
      cost = max_tokens_per_subqueue;
    create_queue(priority)->enqueue(cl, cost, item);
  }

  void enqueue_front(K cl, unsigned priority, unsigned share, T item) { // 1/share internally
    if (share < min_cost)
      share = min_cost;
    if (share > max_tokens_per_subqueue)
      share = max_tokens_per_subqueue;

    create_queue(priority)->enqueue_front(cl, share, item);
  }

  bool empty() const {
    assert(total_priority >= 0);
    assert((total_priority == 0) || !(queue.empty()));
    return queue.empty() && high_queue.empty() && dm_queue.empty();
  }

//  T dequeue_dmClock() {
//    assert(!empty());
//    if (!(high_queue.empty())) {
//      T ret = high_queue.rbegin()->second.front().second;
//      high_queue.rbegin()->second.pop_front();
//      if (high_queue.rbegin()->second.empty())
//	high_queue.erase(high_queue.rbegin()->first);
//      return ret;
//    }

  T dequeue_dmClock(int& tag_type) {
     assert(!empty());
     tag_type = Q_NONE;
     if (!(high_queue.empty())) {
       T ret = high_queue.rbegin()->second.front().second;
       high_queue.rbegin()->second.pop_front();
       if (high_queue.rbegin()->second.empty())
 	high_queue.erase(high_queue.rbegin()->first);
       return ret;
     }
    // priority levels are treated as prop share
    assert(!(dm_queue.empty()));
    std::pair<T, tag_types_t> ret = dm_queue.pop_front();
    tag_type = ret.second;
    return ret.first;
  }

  T dequeue() {
    assert(!empty());

    if (!(high_queue.empty())) {
      T ret = high_queue.rbegin()->second.front().second;
      high_queue.rbegin()->second.pop_front();
      if (high_queue.rbegin()->second.empty())
	high_queue.erase(high_queue.rbegin()->first);
      return ret;
    }

    // if there are multiple buckets/subqueues with sufficient tokens,
    // we behave like a strict priority queue among all subqueues that
    // are eligible to run.
    for (typename SubQueues::iterator i = queue.begin(); i != queue.end();
	++i) {
      assert(!(i->second.empty()));
      if (i->second.front().first < i->second.num_tokens()) {
	T ret = i->second.front().second;
	unsigned cost = i->second.front().first;
	i->second.take_tokens(cost);
	i->second.pop_front();
	if (i->second.empty())
	  remove_queue(i->first);
	distribute_tokens(cost);
	return ret;
      }
    }

    // if no subqueues have sufficient tokens, we behave like a strict
    // priority queue.
    T ret = queue.rbegin()->second.front().second;
    unsigned cost = queue.rbegin()->second.front().first;
    queue.rbegin()->second.pop_front();
    if (queue.rbegin()->second.empty())
      remove_queue(queue.rbegin()->first);
    distribute_tokens(cost);
    return ret;
  }

  void purge_dmClock() {
    dm_queue.purge_idle_clients();
  }

  std::string get_status() {
    return dm_queue.print_status();
  }

  void dump(Formatter *f) const {
    f->dump_int("total_priority", total_priority);
    f->dump_int("max_tokens_per_subqueue", max_tokens_per_subqueue);
    f->dump_int("min_cost", min_cost);
    f->open_array_section("high_queues");
    for (typename SubQueues::const_iterator p = high_queue.begin();
	p != high_queue.end(); ++p) {
      f->open_object_section("subqueue");
      f->dump_int("priority", p->first);
      p->second.dump(f);
      f->close_section();
    }
    f->close_section();
    f->open_array_section("queues");
    for (typename SubQueues::const_iterator p = queue.begin();
	p != queue.end(); ++p) {
      f->open_object_section("subqueue");
      f->dump_int("priority", p->first);
      p->second.dump(f);
      f->close_section();
    }
    f->close_section();
    f->open_object_section("dm_clock");
    dm_queue.dump(f);
    f->close_section();
  }

};

//#pragma pop_macro("dout_prefix")
//#pragma pop_macro("dout_subsys")
#endif
