/*-------------------------------------------------------------------------
 *
 * xact.h
 *	  postgres transaction system definitions
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACT_H
#define XACT_H

#include "access/transam.h"
#include "access/xlog.h"
#include "utils/nabstime.h"
#include "utils/timestamp.h"

/*
 * Xact isolation levels
 */
#define XACT_READ_UNCOMMITTED	0
#define XACT_READ_COMMITTED		1
#define XACT_REPEATABLE_READ	2
#define XACT_SERIALIZABLE		3

extern int	DefaultXactIsoLevel;
extern int	XactIsoLevel;

/*
 * We only implement two distinct levels, so this is a convenience to
 * check which level we're really using internally.
 */
#define IsXactIsoLevelSerializable ((XactIsoLevel == XACT_REPEATABLE_READ || XactIsoLevel == XACT_SERIALIZABLE))

/* Xact read-only state */
extern bool DefaultXactReadOnly;
extern bool XactReadOnly;

/*
 *	transaction states - transaction state from server perspective
 */
typedef enum TransState
{
	TRANS_DEFAULT,
	TRANS_START,
	TRANS_INPROGRESS,
	TRANS_COMMIT,
	TRANS_ABORT
} TransState;

/*
 *	transaction block states - transaction state of client queries
 */
typedef enum TBlockState
{
	TBLOCK_DEFAULT,
	TBLOCK_STARTED,
	TBLOCK_BEGIN,
	TBLOCK_INPROGRESS,
	TBLOCK_END,
	TBLOCK_ABORT,
	TBLOCK_ENDABORT,

	TBLOCK_SUBBEGIN,
	TBLOCK_SUBBEGINABORT,
	TBLOCK_SUBINPROGRESS,
	TBLOCK_SUBEND,
	TBLOCK_SUBABORT,
	TBLOCK_SUBENDABORT_OK,
	TBLOCK_SUBENDABORT_ERROR
} TBlockState;

/*
 *	end-of-transaction cleanup callbacks for dynamically loaded modules
 */
typedef void (*EOXactCallback) (bool isCommit, void *arg);

/*
 *	transaction state structure
 */
typedef struct TransactionStateData
{
	TransactionId	transactionIdData;		/* my XID */
	CommandId		commandId;				/* current CID */
	TransState		state;					/* low-level state */
	TBlockState		blockState;				/* high-level state */
	int				nestingLevel;			/* nest depth */
	MemoryContext	curTransactionContext;	/* my xact-lifetime context */
	List		   *childXids;				/* subcommitted child XIDs */
	AclId			currentUser;			/* subxact start current_user */
	struct TransactionStateData *parent;	/* back link to parent */
} TransactionStateData;

typedef TransactionStateData *TransactionState;


/* ----------------
 *		transaction-related XLOG entries
 * ----------------
 */

/*
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field
 */
#define XLOG_XACT_COMMIT	0x00
#define XLOG_XACT_ABORT		0x20

typedef struct xl_xact_commit
{
	time_t		xtime;
	int			nrels;			/* number of RelFileNodes */
	int			nsubxacts;		/* number of subtransaction XIDs */
	/* Array of RelFileNode(s) to drop at commit */
	RelFileNode	xnodes[1];		/* VARIABLE LENGTH ARRAY */
	/* ARRAY OF COMMITTED SUBTRANSACTION XIDs FOLLOWS */
} xl_xact_commit;

#define MinSizeOfXactCommit	offsetof(xl_xact_commit, xnodes)

typedef struct xl_xact_abort
{
	time_t		xtime;
	int			nrels;			/* number of RelFileNodes */
	int			nsubxacts;		/* number of subtransaction XIDs */
	/* Array of RelFileNode(s) to drop at abort */
	RelFileNode	xnodes[1];		/* VARIABLE LENGTH ARRAY */
	/* ARRAY OF ABORTED SUBTRANSACTION XIDs FOLLOWS */
} xl_xact_abort;

#define MinSizeOfXactAbort offsetof(xl_xact_abort, xnodes)


/* ----------------
 *		extern definitions
 * ----------------
 */
extern bool IsTransactionState(void);
extern bool IsAbortedTransactionBlockState(void);
extern TransactionId GetTopTransactionId(void);
extern TransactionId GetCurrentTransactionId(void);
extern CommandId GetCurrentCommandId(void);
extern AbsoluteTime GetCurrentTransactionStartTime(void);
extern AbsoluteTime GetCurrentTransactionStartTimeUsec(int *usec);
extern int	GetCurrentTransactionNestLevel(void);
extern bool TransactionIdIsCurrentTransactionId(TransactionId xid);
extern void CommandCounterIncrement(void);
extern void StartTransactionCommand(void);
extern void CommitTransactionCommand(void);
extern void AbortCurrentTransaction(void);
extern void BeginTransactionBlock(void);
extern void EndTransactionBlock(void);
extern bool IsSubTransaction(void);
extern bool IsTransactionBlock(void);
extern bool IsTransactionOrTransactionBlock(void);
extern char TransactionBlockStatusCode(void);
extern void UserAbortTransactionBlock(void);
extern void AbortOutOfAnyTransaction(void);
extern void PreventTransactionChain(void *stmtNode, const char *stmtType);
extern void RequireTransactionChain(void *stmtNode, const char *stmtType);
extern bool IsInTransactionChain(void *stmtNode);
extern void RegisterEOXactCallback(EOXactCallback callback, void *arg);
extern void UnregisterEOXactCallback(EOXactCallback callback, void *arg);

extern void RecordTransactionCommit(void);

extern int	xactGetCommittedChildren(TransactionId **ptr, bool metoo);

extern void XactPushRollback(void (*func) (void *), void *data);
extern void XactPopRollback(void);

extern void xact_redo(XLogRecPtr lsn, XLogRecord *record);
extern void xact_undo(XLogRecPtr lsn, XLogRecord *record);
extern void xact_desc(char *buf, uint8 xl_info, char *rec);

#endif   /* XACT_H */
