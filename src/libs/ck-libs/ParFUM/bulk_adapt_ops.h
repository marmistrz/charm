/** Bulk Adapt Operations: An array class that shadows the mesh
    partitions and performs bulk adaptivity operations.  
    Created: 6 Dec 2006 by Terry L. Wilmarth */
#ifndef __BULK_ADAPT_OPS_H
#define __BULK_ADAPT_OPS_H

#include "charm++.h"
#include "ParFUM.h"
#include "ParFUM_internals.h"
#include "ParFUM_SA.h"

/// This shadow array is attached to a partition to perform bulk adaptivity
/** This is a shadow array for performing all bulk adaptivity operations, 
    including cross-partition operations. */
class BulkAdapt {
 private:
  /// The index of the partition this array element is attached to
  int partitionID;
	int meshID;
  /// Pointer to the local mesh partition associated with this array
  FEM_Mesh *meshPtr;
  /// Proxy to ParFUM shadow array attached to each mesh partition
  CProxy_ParFUMShadowArray shadowProxy;
  /// Local element of shadow array
  ParFUMShadowArray *localShadow;

 public:
  /// Construct array to be attached to the partitions of mesh mId
  BulkAdapt(int meshid, FEM_Mesh *mPtr, int partID, CProxy_ParFUMShadowArray sa_proxy);
  /// Destructor
  ~BulkAdapt();

  /// Pack/Unpack this array element
  void pup(PUP::er &p);

  /* BASIC DATA ACCESS FUNCTIONS */
  
  /// Return this partition's ID
  int getPartition() { return partitionID; }
  /// Return a pointer to the local mesh partition
  FEM_Mesh *getMeshPtr() { return meshPtr; }

  /* BULK MESH OPERATIONS: These are all called locally, but may invoke
     remote operations. */

  /// Perform an edge bisection (2D and 3D).
  /** Locks mesh and any affected IDXL lists, performs operation,
      updates adapt adjacencies, and unlocks mesh & IDXL
      lists. Returns zero if the lock fails, positive if the operation
      suceeds, and negative if the operations fails for some other
      reason. */
  int edge_bisect(int elemID, int elemType, int edgeID, int dim);
  int edge_bisect_2D(int elemID, int elemType, int edgeID);
  int edge_bisect_3D(int elemID, int elemType, int edgeID);

	//TODO: add elemType to the prototype of all the following mesh-modification functions
  /// Perform an edge flip (2D)
  /** Locks mesh and any affected IDXL lists, performs operation,
      updates adapt adjacencies, and unlocks mesh & IDXL
      lists. Returns zero if the lock fails, positive if the operation
      suceeds, and negative if the operations fails for some other
      reason. */
  int edge_flip(int elemID, int edgeID);

  /// Perform a Delaunay 2-3 flip (3D)
  /** Locks mesh and any affected IDXL lists, performs operation,
      updates adapt adjacencies, and unlocks mesh & IDXL
      lists. Returns zero if the lock fails, positive if the operation
      suceeds, and negative if the operations fails for some other
      reason. */
  int flip_23(int elemID, int faceID);

  /// Perform a Delaunay 3-2 flip (3D)
  /** Locks mesh and any affected IDXL lists, performs operation,
      updates adapt adjacencies, and unlocks mesh & IDXL
      lists. Returns zero if the lock fails, positive if the operation
      suceeds, and negative if the operations fails for some other
      reason. */
  int flip_32(int elemID, int edgeID);

  /// Perform an edge collapse (2D and 3D)
  /** Locks mesh and any affected IDXL lists, performs operation,
      updates adapt adjacencies, and unlocks mesh & IDXL
      lists. Returns zero if the lock fails, positive if the operation
      suceeds, and negative if the operations fails for some other
      reason. */
  int edge_collapse(int elemID, int edgeID);

  /// Perform a single side of an edge_bisect operation
  void one_side_split_2D(adaptAdj &startElem, adaptAdj &splitElem, int edgeID,
			 int *node1idx, int *node2idx, int *newNodeID,
			 adaptAdj **startElemAdaptAdj, adaptAdj **splitElemAdaptAdj, bool startSide);


  /* COMMUNICATION HELPERS FOR BULK ADAPTIVITY OPERATIONS */
  
  /* LOCAL HELPERS FOR BULK ADAPTIVITY OPERATIONS */
	
	/** Add a new element to the mesh. 
	 * Update its connectivity
	 * Return index of new element
	 * */
	int add_element(int elemType,int nodesPerElem,int *conn);

	/** Update the conn of an element*/
	void update_element_conn(int elemType,int elemID,int nodesPerElem,int *conn);



	/**
	 * Add a new node to the mesh
	 * update its co-ordinates 
	 * Return index of new node
	 * */
	int add_node(int dim,double *coords);
	
	/** Update the co-ordimates of the given node
	*/
	void update_node_coord(int nodeID,int dim,double *coords);

	void make_node_shared(int nodeID,int numSharedChunks,int *sharedChunks);
};

void midpoint(double *n1, double *n2, int dim, double *result);

int getRelNode(int nodeIdx, int *conn, int nodesPerElem);

int getEdgeID(int node1, int node2, int nodePerElem, int dim);

#endif
