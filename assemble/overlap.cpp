//(c) 2016 by Authors
//This file is a part of ABruijn program.
//Released under the BSD license (see LICENSE file)

#include <set>
#include <iostream>
#include <cassert>
#include <algorithm>
#include <thread>

#include "overlap.h"

void OverlapDetector::findAllOverlaps(size_t numThreads)
{
	Logger::get().info() << "Finding overlaps:";
	_overlapMatrix.clear();
	_jobQueue.clear();
	_nextJob = 0;

	for (const auto& seqHash : _seqContainer.getIndex())
	{
		_jobQueue.push_back(seqHash.first);
	}

	std::vector<std::thread> threads(numThreads);
	for (size_t i = 0; i < threads.size(); ++i)
	{
		threads[i] = std::thread(&OverlapDetector::parallelWorker, this);
	}
	for (size_t i = 0; i < threads.size(); ++i)
	{
		threads[i].join();
	}

	_overlapMatrix.clear();
}


void OverlapDetector::parallelWorker()
{
	_fetchMutex.lock();
	while (true)
	{
		if (_nextJob == _jobQueue.size())
		{
			_fetchMutex.unlock();
			return;
		}
		_progress.advance();
		FastaRecord::Id readId = _jobQueue[_nextJob++];
		_overlapIndex[readId];	//empty vector by default
		if (_seqContainer.seqLen(readId) < (size_t)_minimumOverlap) 
			continue;

		_fetchMutex.unlock();
		auto detectedOverlaps = this->getReadOverlaps(readId);
		_fetchMutex.lock();
		
		for (auto ovlp : detectedOverlaps)
		{
			if (_overlapMatrix.count(std::make_tuple(ovlp.curId, ovlp.extId)))
			{
				continue;
			}
			//detected overlap
			_overlapMatrix.insert(std::make_tuple(ovlp.curId, ovlp.extId));
			_overlapIndex[ovlp.curId].push_back(ovlp);

			//in opposite direction
			ovlp.reverse();
			_overlapMatrix.insert(std::make_tuple(ovlp.curId, ovlp.extId));
			_overlapIndex[ovlp.curId].push_back(ovlp);

			//on reverse strands
			auto curLen = _seqContainer.seqLen(ovlp.curId);
			auto extLen = _seqContainer.seqLen(ovlp.extId);
			ovlp.complement(curLen, extLen);
			_overlapMatrix.insert(std::make_tuple(ovlp.curId, ovlp.extId));
			_overlapIndex[ovlp.curId].push_back(ovlp);

			//opposite again
			ovlp.reverse();
			_overlapMatrix.insert(std::make_tuple(ovlp.curId, ovlp.extId));
			_overlapIndex[ovlp.curId].push_back(ovlp);
		}
	}
}


//pre-filtering
bool OverlapDetector::goodStart(int32_t curPos, int32_t extPos, 
								int32_t curLen, int32_t extLen) const
{	
	return std::min(curPos, extPos) < _maximumOverhang && 
		   extPos < extLen - _minimumOverlap &&
		   curPos < curLen - _minimumOverlap;
}

OverlapDetector::JumpRes 
OverlapDetector::jumpTest(int32_t curPrev, int32_t curNext,
						  int32_t extPrev, int32_t extNext) const
{
	static const int CLOSE_FRAC = 8;
	static const int FAR_FRAC = 2;
	if (curNext - curPrev > _maximumJump) return J_END;

	if (0 < curNext - curPrev && curNext - curPrev < _maximumJump && 
		0 < extNext - extPrev && extNext - extPrev < _maximumJump)
	{
		if (abs((curNext - curPrev) - (extNext - extPrev)) 
			< _maximumJump / CLOSE_FRAC)
			return J_CLOSE;
		if (abs((curNext - curPrev) - (extNext - extPrev)) 
			< _maximumJump / FAR_FRAC)
			return J_FAR;
	}
	return J_INCONS;
}


//Check if it is a proper overlap
bool OverlapDetector::overlapTest(const OverlapRange& ovlp, int32_t curLen, 
								  int32_t extLen) const
{
	if (ovlp.curRange() < _minimumOverlap || ovlp.extRange() < _minimumOverlap)
		return false;
	if (abs(ovlp.curRange() - ovlp.extRange()) > _maximumJump)
		return false;
	if (std::min(ovlp.curBegin, ovlp.extBegin) > _maximumOverhang)
		return false;
	if (std::min(curLen - ovlp.curEnd, extLen - ovlp.extEnd) > _maximumOverhang)
		return false;
	return true;
}

//Getting all possible overlaps
//based on the shared kmers (common jump-paths)
std::vector<OverlapRange> 
OverlapDetector::getReadOverlaps(FastaRecord::Id currentReadId) const
{
	const int MAX_PATHS = 100;

	const auto& readIndex = _vertexIndex.getIndexByRead();
	const auto& kmerIndex = _vertexIndex.getIndexByKmer();
	if (!readIndex.count(currentReadId)) return std::vector<OverlapRange>();
	
	std::unordered_map<FastaRecord::Id, 
					   std::vector<OverlapRange>> activePaths;
		
	auto curLen = _seqContainer.seqLen(currentReadId);
	//for all kmers in this read
	for (const auto& curKmerPos : readIndex.at(currentReadId))
	{
		int32_t curPos = curKmerPos.position;
		//for all other occurences of this kmer (extension candidates)
		for (const auto& extReadPos : kmerIndex.at(curKmerPos.kmer))
		{
			//don't want self-overlaps
			if (extReadPos.readId == currentReadId) continue;
			//maybe it's already processed
			//if (_overlapMatrix.count(std::make_tuple(extReadPos.readId,
			//				   						 currentReadId))) continue;

			int32_t extPos = extReadPos.position;
			auto& extPaths = activePaths[extReadPos.readId];
			auto extLen = _seqContainer.seqLen(extReadPos.readId);
			if (extLen < (size_t)_minimumOverlap) continue;

			//searching for longest possible extension
			size_t maxCloseId = 0;
			size_t maxFarId = 0;
			int maxCloseLen = 0;
			int maxFarLen = 0;
			bool extendsClose = false;
			bool extendsFar = false;
			std::set<size_t> eraseMarks;
			for (size_t pathId = 0; pathId < extPaths.size(); ++pathId)
			{
				int32_t jumpLength = curPos - extPaths[pathId].curEnd;
				JumpRes jumpResult = this->jumpTest(extPaths[pathId].curEnd, curPos,
													extPaths[pathId].extEnd, extPos);

				switch (jumpResult)
				{
					case J_END:
						break;
					case J_INCONS:
						break;
					case J_CLOSE:
						eraseMarks.insert(pathId);
						if (jumpLength > maxCloseLen)
						{
							extendsClose = true;
							maxCloseId = pathId;	
							maxCloseLen = curPos - extPaths[maxCloseId].curEnd;
						}
						break;
					case J_FAR:
						if (jumpLength > maxFarLen)
						{
							extendsFar = true;
							maxFarId = pathId;
							maxFarLen = curPos - extPaths[maxFarId].curEnd;
						}
						break;
				}
			}
			//update the best close extension
			if (extendsClose)
			{
				eraseMarks.erase(maxCloseId);
				extPaths[maxCloseId].curEnd = curPos;
				extPaths[maxCloseId].extEnd = extPos;
			}
			//update the best far extension, keep the old path as a copy
			if (extendsFar)
			{
				extPaths.push_back(extPaths[maxFarId]);
				extPaths.back().curEnd = curPos;
				extPaths.back().extEnd = extPos;
			}
			//if no extensions possible (or there are no active paths), start a new path
			if (!extendsClose && !extendsFar && 
				this->goodStart(curPos, extPos, curLen, extLen))
			{
				extPaths.push_back(OverlapRange(currentReadId, extReadPos.readId,
												curPos, extPos));
			}
			//keep at most MAX_PATHS paths
			if (extPaths.size() > MAX_PATHS)
			{
				size_t shortestId = 0;
				int32_t shortestLength = extPaths[shortestId].curRange();
				for (size_t i = 0; i < extPaths.size(); ++i)
				{
					if (extPaths[i].curRange() < shortestLength)
					{
						shortestLength = extPaths[i].curRange();
						shortestId = i;
					}
				}
				eraseMarks.insert(shortestId);
			}
			//cleaning up
			for (auto itEraseId = eraseMarks.rbegin(); 
				 itEraseId != eraseMarks.rend(); ++itEraseId)
			{
				extPaths[*itEraseId] = extPaths.back();
				extPaths.pop_back();
			}
		} //end loop over kmer occurences in other reads
	} //end loop over kmers in the current read
	

	std::vector<OverlapRange> detectedOverlaps;
	std::vector<OverlapRange> debugOverlaps;
	for (const auto& ap : activePaths)
	{
		//if (ap.second.size() > 100)
		//	DEBUG_PRINT("Pathset length: " << ap.second.size());
		auto extLen = _seqContainer.seqLen(ap.first);
		OverlapRange maxOverlap(0, 0, 0, 0);
		OverlapRange outputOverlap(0, 0, 0, 0);
		bool passedTest = false;
		for (auto& ovlp : ap.second)
		{
			if (this->overlapTest(ovlp, curLen, extLen))
			{
				passedTest = true;
				if (maxOverlap.curRange() < ovlp.curRange()) maxOverlap = ovlp;
			}
			if (outputOverlap.curRange() < ovlp.curRange()) outputOverlap = ovlp;
		}

		if (outputOverlap.curRange() > 1000)
		{
			debugOverlaps.push_back(outputOverlap);
		}

		if (passedTest)
		{
			this->addOverlapShifts(maxOverlap);
			detectedOverlaps.push_back(std::move(maxOverlap));
		}
	}


	if (!debugOverlaps.empty())
	{
		_logMutex.lock();
		Logger::get().debug() << "Ovlps for " 
					<< _seqContainer.getIndex().at(currentReadId).description
					<< " " << readIndex.at(currentReadId).size();
		for (auto& ovlp : debugOverlaps)
		{
			Logger::get().debug() << "\t" 
					<< _seqContainer.getIndex().at(ovlp.extId).description
					<< "\tcs:" << ovlp.curBegin << "\tcl:" << ovlp.curRange()
					<< "\tes:" << ovlp.extBegin << "\tel:" << ovlp.extRange();
		}
		_logMutex.unlock();
	}

	return detectedOverlaps;
}

namespace
{
	template<typename T>
	T median(std::vector<T>& vec)
	{
		std::sort(vec.begin(), vec.end());
		//NOTE: there's a bug in libstdc++ nth_element, 
		//that sometimes leads to a segfault
		//std::nth_element(vec.begin(), vec.begin() + vec.size() / 2, 
		//				 vec.end());
		return vec[vec.size() / 2];
	}
}

void OverlapDetector::addOverlapShifts(OverlapRange& ovlp) const
{
	//get shared kmers inside the overlap
	std::vector<int32_t> ovlpShifts;
	for (const auto& curKmer : _vertexIndex.getIndexByRead().at(ovlp.curId))
	{
		if (ovlp.curBegin <= curKmer.position &&
			curKmer.position <= ovlp.curEnd)
		{
			for (const auto& extKmer : _vertexIndex.getIndexByKmer().at(curKmer.kmer))
			{
				if (extKmer.readId == ovlp.extId &&
				    ovlp.extBegin <= extKmer.position &&
					extKmer.position <= ovlp.extEnd)
				{
					ovlpShifts.push_back(curKmer.position - extKmer.position);
				}
			}
		}
	}

	ovlp.leftShift = median(ovlpShifts);
	ovlp.rightShift = _seqContainer.seqLen(ovlp.extId) - 
					  _seqContainer.seqLen(ovlp.curId) + ovlp.leftShift;
}
