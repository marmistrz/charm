#ifndef _IDL_H_
#define _IDL_H_

#include "charm++.h"
#include "idl.top.h"

#define CI_PE_ANY         CK_PE_ANY
#define CI_PE_ALL         CK_PE_ALL
#define CI_PE_ALL_BUT_ME  CK_PE_ALL_BUT_ME

class CIMsgEmpty : public comm_object {
};

class CIHandle {
  private:
    int chare;
    int proc;
    union {
      ChareIDType cid;
      GroupIdType gid;
    } u;
  public:
   CIHandle() {
     chare = 1;
     proc = CI_PE_ANY;
   }
   CIHandle(const CIHandle &hndl) {
     chare = hndl.chare;
     proc = hndl.proc;
     u = hndl.u;
   }
   void ciSetProc(int _proc) { proc = _proc; }
   int ciGetProc(void) { return proc; }
   // For ITC++ -> IDL Interoperability
   CIHandle(ChareIDType _cid) : chare(1) { u.cid = _cid; }
   CIHandle(GroupIdType _gid) : chare(0) { u.gid = _gid; }
   void setCID(ChareIDType _cid) { chare=1; u.cid = _cid; }
   void setGID(int _gid) { chare=0; u.gid = _gid; }
   // For IDL -> ITC++ Interoperability
   int isChare(void) { return chare; }
   ChareIDType ciCID(void) { return u.cid; }
   GroupIdType ciGID(void) { return u.gid; }
};

class CIMethodParams {
  private:
    int prioWords;
    int prio;
    int *prioVec;
    int synch;
  public:
    CIMethodParams() : prioWords(0), prioVec(0), synch(0) {}
    void ciSetPrioWords(int words) { prioWords = words; }
    void ciSetPrioVec(int *_vec) { prioVec = _vec; }
    void ciSetPrio(int words, int *_vec) { prioWords=words; prioVec = _vec; }
    void ciSetIntPrio(int _prio) { prioWords=sizeof(int); prio = _prio; }
    void ciSetSynch(int _synch) { synch = _synch; }
};

extern CIHandle ciThisHandle(void *);

#endif
