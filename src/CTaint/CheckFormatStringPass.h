/*
 * CheckFormatStringPass.h
 *
 *  Created on: 2014-05-29
 *      Author: noundou
 */

#ifndef CHECKFORMATSTRINGPASS_H_
#define CHECKFORMATSTRINGPASS_H_

#include "CForwardFlowAnalysis.h"
#include "CTaintAnalysis.h"

namespace {
class CheckFormatStringPass : public CForwardFlowAnalysis {

public:
	CheckFormatStringPass(CTaintAnalysis *taintAnalysis);
	void doAnalysis();

	//******* Implementation of methods inherited from CDataFlow *******//
	virtual bool merge(BasicBlock *curBB, BasicBlock *succBB){ return false;}
	virtual void mergeCopyPredOutFlowToInFlow(Instruction &predInst, Instruction &curInst){}

	//******* Implementation of visit methods *******//
	virtual void visitLoadInst(LoadInst &I){}
	virtual void visitStoreInst(StoreInst &I){}
	virtual void visitCallInst(CallInst &I);
	virtual void visitReturnInst(ReturnInst &I){}

protected:
	CTaintAnalysis *_taintAnalysis;
};
}

CheckFormatStringPass::CheckFormatStringPass(CTaintAnalysis *taintAnalysis)
:CForwardFlowAnalysis(taintAnalysis->getAllProcsTPOrder()),
 _taintAnalysis(taintAnalysis)
{

}


void CheckFormatStringPass::doAnalysis()
{
	errs() << "## Starting format string checking pass\n";
	analyze();
}

inline void CheckFormatStringPass::visitCallInst(CallInst &I) {
	_taintAnalysis->visitCallInstSink(I);
}

#endif /* CHECKFORMATSTRINGPASS_H_ */