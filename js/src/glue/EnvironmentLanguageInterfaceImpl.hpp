/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 1991, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#ifndef ENVIRONMENTLANGUAGEINTERFACEIMPL_HPP_
#define ENVIRONMENTLANGUAGEINTERFACEIMPL_HPP_

#include "omr.h"

#include "EnvironmentLanguageInterface.hpp"

#include "EnvironmentBase.hpp"

class MM_EnvironmentLanguageInterfaceImpl : public MM_EnvironmentLanguageInterface
{
private:
protected:
public:

private:
protected:
	virtual bool initialize(MM_EnvironmentBase *env);
	virtual void tearDown(MM_EnvironmentBase *env);

	MM_EnvironmentLanguageInterfaceImpl(MM_EnvironmentBase *env);

public:
	static MM_EnvironmentLanguageInterfaceImpl *newInstance(MM_EnvironmentBase *env);
	virtual void kill(MM_EnvironmentBase *env);

	static MM_EnvironmentLanguageInterfaceImpl *getInterface(MM_EnvironmentLanguageInterface *linterface) { return (MM_EnvironmentLanguageInterfaceImpl *)linterface; }

	/**
	 * Acquire exclusive VM access. This must lock the VM thread list mutex (OMR_VM::_vmThreadListMutex).
	 *
	 * This method may be called by a thread that already holds exclusive VM access. In that case, the
	 * OMR_VMThread::exclusiveCount counter is incremented (without reacquiring lock on VM thread list
	 * mutex).
	 */
	virtual void
	acquireExclusiveVMAccess()
	{
		if (0 == _omrThread->exclusiveCount) {
			omrthread_monitor_enter(_env->getOmrVM()->_vmThreadListMutex);
		}
		_omrThread->exclusiveCount += 1;
	}

	/**
	 * Try and acquire exclusive access if no other thread is already requesting it.
	 * Make an attempt at acquiring exclusive access if the current thread does not already have it.  The
	 * attempt will abort if another thread is already going for exclusive, which means this
	 * call can return without exclusive access being held.  As well, this call will block for any other
	 * requesting thread, and so should be treated as a safe point.
	 * @note call can release VM access.
	 * @return true if exclusive access was acquired, false otherwise.
	 */
	virtual bool
	tryAcquireExclusiveVMAccess()
	{
		this->acquireExclusiveVMAccess();
		return true;
	}

	/**
	 * Releases exclusive VM access.
	 */
	virtual void
	releaseExclusiveVMAccess()
	{
		Assert_MM_true(0 < _omrThread->exclusiveCount);
		_omrThread->exclusiveCount -= 1;
		if (0 == _omrThread->exclusiveCount) {
			omrthread_monitor_exit(_env->getOmrVM()->_vmThreadListMutex);
		}
	}

#if defined (OMR_GC_THREAD_LOCAL_HEAP)
	virtual void disableInlineTLHAllocate();
	virtual void enableInlineTLHAllocate();
	virtual bool isInlineTLHAllocateEnabled();
#endif /* OMR_GC_THREAD_LOCAL_HEAP */

	virtual void parallelMarkTask_setup(MM_EnvironmentBase *env);
	virtual void parallelMarkTask_cleanup(MM_EnvironmentBase *env);


        virtual void acquireVMAccess() ;
        virtual void releaseVMAccess();
        virtual uintptr_t relinquishExclusiveVMAccess();
        virtual void assumeExclusiveVMAccess(uintptr_t exclusiveCount);
        virtual bool isExclusiveAccessRequestWaiting();
};

#endif /* ENVIRONMENTLANGUAGEINTERFACEIMPL_HPP_ */
