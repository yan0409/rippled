
#include <iostream>
#include <fstream>

#include <boost/lexical_cast.hpp>
#include <boost/make_shared.hpp>

#include "Application.h"
#include "Ledger.h"
#include "utils.h"
#include "../obj/src/newcoin.pb.h"
#include "PackedMessage.h"
#include "Config.h"
#include "Conversion.h"
#include "BitcoinUtil.h"
#include "Wallet.h"
#include "BinaryFormats.h"
#include "LedgerTiming.h"

Ledger::Ledger(const NewcoinAddress& masterID, uint64 startAmount) : mTotCoins(startAmount),
	mCloseTime(0), mLedgerSeq(0), mLedgerInterval(LEDGER_INTERVAL), mClosed(false), mValidHash(false),
	mAccepted(false), mImmutable(false)
{
	mTransactionMap = boost::make_shared<SHAMap>();
	mAccountStateMap = boost::make_shared<SHAMap>();

	// special case: put coins in root account
	AccountState::pointer startAccount = boost::make_shared<AccountState>(masterID);
	startAccount->peekSLE().setIFieldAmount(sfBalance, startAmount);
	startAccount->peekSLE().setIFieldU32(sfSequence, 1);
	writeBack(lepCREATE, startAccount->getSLE());
#if 0
	std::cerr << "Root account:";
	startAccount->dump();
#endif
}

Ledger::Ledger(const uint256 &parentHash, const uint256 &transHash, const uint256 &accountHash,
	uint64 totCoins, uint64 timeStamp, uint32 ledgerSeq)
		: mParentHash(parentHash), mTransHash(transHash), mAccountHash(accountHash),
		mTotCoins(totCoins), mCloseTime(timeStamp), mLedgerSeq(ledgerSeq), mLedgerInterval(LEDGER_INTERVAL),
		mClosed(false), mValidHash(false), mAccepted(false), mImmutable(false)
{
	updateHash();
}

Ledger::Ledger(Ledger::pointer prevLedger) : mParentHash(prevLedger->getHash()), mTotCoins(prevLedger->mTotCoins),
	mLedgerSeq(prevLedger->mLedgerSeq + 1), mLedgerInterval(prevLedger->mLedgerInterval),
	mClosed(false), mValidHash(false), mAccepted(false), mImmutable(false),
	mTransactionMap(new SHAMap()), mAccountStateMap(prevLedger->mAccountStateMap)
{
	prevLedger->setClosed();
	prevLedger->updateHash();
	mAccountStateMap->setSeq(mLedgerSeq);
	mCloseTime = prevLedger->getNextLedgerClose();
}

Ledger::Ledger(const std::vector<unsigned char>& rawLedger) : mCloseTime(0),
	mLedgerSeq(0), mClosed(false), mValidHash(false), mAccepted(false), mImmutable(true)
{
	Serializer s(rawLedger);
	// 32seq, 64fee, 256phash, 256thash, 256ahash, 64ts
	if (!s.get32(mLedgerSeq, BLgPIndex)) return;
	if (!s.get64(mTotCoins, BLgPTotCoins)) return;
	if (!s.get256(mParentHash, BLgPPrevLg)) return;
	if (!s.get256(mTransHash, BLgPTxT)) return;
	if (!s.get256(mAccountHash, BLgPAcT)) return;
	if (!s.get64(mCloseTime, BLgPClTs)) return;
	if (!s.get16(mLedgerInterval, BLgPNlIn)) return;
	updateHash();
	if(mValidHash)
	{
		mTransactionMap = boost::make_shared<SHAMap>();
		mAccountStateMap = boost::make_shared<SHAMap>();
	}
}

Ledger::Ledger(const std::string& rawLedger) : mCloseTime(0),
	mLedgerSeq(0), mClosed(false), mValidHash(false), mAccepted(false), mImmutable(true)
{
	Serializer s(rawLedger);
	// 32seq, 64fee, 256phash, 256thash, 256ahash, 64ts
	if (!s.get32(mLedgerSeq, BLgPIndex)) return;
	if (!s.get64(mTotCoins, BLgPTotCoins)) return;
	if (!s.get256(mParentHash, BLgPPrevLg)) return;
	if (!s.get256(mTransHash, BLgPTxT)) return;
	if (!s.get256(mAccountHash, BLgPAcT)) return;
	if (!s.get64(mCloseTime, BLgPClTs)) return;
	if (!s.get16(mLedgerInterval, BLgPNlIn)) return;
	updateHash();
	if(mValidHash)
	{
		mTransactionMap = boost::make_shared<SHAMap>();
		mAccountStateMap = boost::make_shared<SHAMap>();
	}
}

void Ledger::updateHash()
{
	if(!mImmutable)
	{
		if (mTransactionMap) mTransHash = mTransactionMap->getHash();
		else mTransHash.zero();
		if (mAccountStateMap) mAccountHash = mAccountStateMap->getHash();
		else mAccountHash.zero();
	}

	Serializer s(116);
	addRaw(s);
	mHash = s.getSHA512Half();
	mValidHash = true;
}

void Ledger::addRaw(Serializer &s)
{
	s.add32(mLedgerSeq);
	s.add64(mTotCoins);
	s.add256(mParentHash);
	s.add256(mTransHash);
	s.add256(mAccountHash);
	s.add64(mCloseTime);
	s.add16(mLedgerInterval);
}

AccountState::pointer Ledger::getAccountState(const NewcoinAddress& accountID)
{
#ifdef DEBUG
//	std::cerr << "Ledger:getAccountState(" << accountID.humanAccountID() << ")" << std::endl;
#endif
	ScopedLock l(mAccountStateMap->Lock());
	SHAMapItem::pointer item = mAccountStateMap->peekItem(Ledger::getAccountRootIndex(accountID));
	if (!item)
	{
#ifdef DEBUG
//		std::cerr << " notfound" << std::endl;
#endif
		return AccountState::pointer();
	}
	SerializedLedgerEntry::pointer sle =
		boost::make_shared<SerializedLedgerEntry>(item->peekSerializer(), item->getTag());
	if (sle->getType() != ltACCOUNT_ROOT) return AccountState::pointer();
	return boost::make_shared<AccountState>(sle);
}

RippleState::pointer Ledger::getRippleState(const uint256& uNode)
{
	ScopedLock l(mAccountStateMap->Lock());
	SHAMapItem::pointer item = mAccountStateMap->peekItem(uNode);
	if (!item)
	{
		return RippleState::pointer();
	}

	SerializedLedgerEntry::pointer sle =
		boost::make_shared<SerializedLedgerEntry>(item->peekSerializer(), item->getTag());
	if (sle->getType() != ltRIPPLE_STATE) return RippleState::pointer();
	return boost::make_shared<RippleState>(sle);
}

bool Ledger::addTransaction(Transaction::pointer trans)
{ // low-level - just add to table, debit fee
	assert(!mAccepted);
	assert(!!trans->getID());
	Serializer s;
	trans->getSTransaction()->add(s);
	SHAMapItem::pointer item = boost::make_shared<SHAMapItem>(trans->getID(), s.peekData());
	if (!mTransactionMap->addGiveItem(item, true)) return false;
	mTotCoins -= trans->getFee();
	return true;
}

bool Ledger::addTransaction(const uint256& txID, const Serializer& txn, STAmount saPaid)
{ // low-level - just add to table
	SHAMapItem::pointer item = boost::make_shared<SHAMapItem>(txID, txn.peekData());
	if (!mTransactionMap->addGiveItem(item, true)) return false;
	mTotCoins -= saPaid;
	return true;
}

bool Ledger::hasTransaction(const uint256& transID) const
{
	return mTransactionMap->hasItem(transID);
}

Transaction::pointer Ledger::getTransaction(const uint256& transID) const
{
	SHAMapItem::pointer item = mTransactionMap->peekItem(transID);
	if (!item) return Transaction::pointer();

	Transaction::pointer txn = theApp->getMasterTransaction().fetch(transID, false);
	if (txn) return txn;

	txn = Transaction::sharedTransaction(item->getData(), true);
	if (txn->getStatus() == NEW)
		txn->setStatus(mClosed ? COMMITTED : INCLUDED, mLedgerSeq);

	theApp->getMasterTransaction().canonicalize(txn, false);
	return txn;
}

bool Ledger::unitTest()
{
	return true;
}

uint256 Ledger::getHash()
{
	if(!mValidHash) updateHash();
	return(mHash);
}

void Ledger::saveAcceptedLedger(Ledger::pointer ledger)
{
	std::string sql="INSERT INTO Ledgers "
		"(LedgerHash,LedgerSeq,PrevHash,TotalCoins,ClosingTime,AccountSetHash,TransSetHash) VALUES ('";
	sql.append(ledger->getHash().GetHex());
	sql.append("','");
	sql.append(boost::lexical_cast<std::string>(ledger->mLedgerSeq));
	sql.append("','");
	sql.append(ledger->mParentHash.GetHex());
	sql.append("','");
	sql.append(boost::lexical_cast<std::string>(ledger->mTotCoins));
	sql.append("','");
	sql.append(boost::lexical_cast<std::string>(ledger->mCloseTime));
	sql.append("','");
	sql.append(ledger->mAccountHash.GetHex());
	sql.append("','");
	sql.append(ledger->mTransHash.GetHex());
	sql.append("');");

	ScopedLock sl(theApp->getLedgerDB()->getDBLock());
	theApp->getLedgerDB()->getDB()->executeSQL(sql);

	// write out dirty nodes
	while(ledger->mTransactionMap->flushDirty(64, TRANSACTION_NODE, ledger->mLedgerSeq))
	{ ; }
	while(ledger->mAccountStateMap->flushDirty(64, ACCOUNT_NODE, ledger->mLedgerSeq))
	{ ; }

}

Ledger::pointer Ledger::getSQL(const std::string& sql)
{
	uint256 ledgerHash, prevHash, accountHash, transHash;
	uint64 totCoins, closingTime;
	uint32 ledgerSeq;
	std::string hash;

	if(1)
	{
		ScopedLock sl(theApp->getLedgerDB()->getDBLock());
		Database *db = theApp->getLedgerDB()->getDB();

		if (!db->executeSQL(sql) || !db->startIterRows())
			 return Ledger::pointer();

		db->getStr("LedgerHash", hash);
		ledgerHash.SetHex(hash);
		db->getStr("PrevHash", hash);
		prevHash.SetHex(hash);
		db->getStr("AccountSetHash", hash);
		accountHash.SetHex(hash);
		db->getStr("TransSetHash", hash);
		transHash.SetHex(hash);
		totCoins = db->getBigInt("TotalCoins");
		closingTime = db->getBigInt("ClosingTime");
		ledgerSeq = db->getBigInt("LedgerSeq");
		db->endIterRows();
	}

	Ledger::pointer ret=boost::make_shared<Ledger>(prevHash, transHash, accountHash, totCoins, closingTime, ledgerSeq);
	if (ret->getHash() != ledgerHash)
	{
		assert(false);
		return Ledger::pointer();
	}
	return ret;
}

Ledger::pointer Ledger::loadByIndex(uint32 ledgerIndex)
{
	std::string sql="SELECT * from Ledgers WHERE LedgerSeq='";
	sql.append(boost::lexical_cast<std::string>(ledgerIndex));
	sql.append("';");
	return getSQL(sql);
}

Ledger::pointer Ledger::loadByHash(const uint256& ledgerHash)
{
	std::string sql="SELECT * from Ledgers WHERE LedgerHash='";
	sql.append(ledgerHash.GetHex());
	sql.append("';");
	return getSQL(sql);
}

void Ledger::addJson(Json::Value& ret)
{
	Json::Value ledger(Json::objectValue);

	boost::recursive_mutex::scoped_lock sl(mLock);
	ledger["ParentHash"] = mParentHash.GetHex();

	if(mClosed)
	{
		ledger["Hash"] = mHash.GetHex();
		ledger["TransactionHash"] = mTransHash.GetHex();
		ledger["AccountHash"] = mAccountHash.GetHex();
		ledger["Closed"] = true;
		ledger["Accepted"] = mAccepted;
		ledger["TotalCoins"] = boost::lexical_cast<std::string>(mTotCoins);
	}
	else ledger["Closed"] = false;
	if (mCloseTime != 0)
		ledger["CloseTime"] = boost::posix_time::to_simple_string(ptFromSeconds(mCloseTime));
	ret[boost::lexical_cast<std::string>(mLedgerSeq)] = ledger;
}

Ledger::pointer Ledger::switchPreviousLedger(Ledger::pointer oldPrevious, Ledger::pointer newPrevious, int limit)
{
	// Build a new ledger that can replace this ledger as the active ledger,
	// with a different previous ledger. We assume our ledger is trusted, as is its
	// previous ledger. We make no assumptions about the new previous ledger.

	int count;

	// 1) Validate sequences and make sure the specified ledger is a valid prior ledger
	if (newPrevious->getLedgerSeq() != oldPrevious->getLedgerSeq()) return Ledger::pointer();

	// 2) Begin building a new ledger with the specified ledger as previous.
	Ledger::pointer newLedger = boost::make_shared<Ledger>(newPrevious);

	// 3) For any transactions in our previous ledger but not in the new previous ledger, add them to the set
	SHAMap::SHAMapDiff mapDifferences;
	std::map<uint256, std::pair<Transaction::pointer, Transaction::pointer> > TxnDiff;
	if (!newPrevious->mTransactionMap->compare(oldPrevious->mTransactionMap, mapDifferences, limit))
		return Ledger::pointer();
	if (!Transaction::convertToTransactions(oldPrevious->getLedgerSeq(), newPrevious->getLedgerSeq(),
			false, true, mapDifferences, TxnDiff))
		return Ledger::pointer(); // new previous ledger contains invalid transactions

	// 4) Try to add those transactions to the new ledger.
	do
	{
		count = 0;
		std::map<uint256, std::pair<Transaction::pointer, Transaction::pointer> >::iterator it = TxnDiff.begin();
		while (it != TxnDiff.end())
		{
			Transaction::pointer& tx = it->second.second;
			if (!tx || newLedger->addTransaction(tx)) // FIXME: addTransaction doesn't do checks
			{
				++count;
				TxnDiff.erase(it++);
			}
			else ++it;
		}
	} while (count != 0);

	// WRITEME: Handle rejected transactions left in TxnDiff

	// 5) Try to add transactions from this ledger to the new ledger.
	std::map<uint256, Transaction::pointer> txnMap;
	for (SHAMapItem::pointer mit = peekTransactionMap()->peekFirstItem();
			!!mit; mit = peekTransactionMap()->peekNextItem(mit->getTag()))
	{
		uint256 txnID = mit->getTag();
		Transaction::pointer tx = theApp->getMasterTransaction().fetch(txnID, false);
		if(!tx) tx = Transaction::sharedTransaction(mit->peekData(), false);
		txnMap.insert(std::make_pair(txnID, tx));
	}

	do
	{
		count = 0;
		std::map<uint256, Transaction::pointer>::iterator it = txnMap.begin();
		while (it != txnMap.end())
		{
			if (newLedger->addTransaction(it->second)) // FIXME: addTransaction doesn't do checks
			{
				++count;
				txnMap.erase(it++);
			}
			else ++it;
		}
	} while(count != 0);


	// WRITEME: Handle rejected transactions left in txnMap

	return Ledger::pointer(newLedger);
}

void Ledger::setAcquiring(void)
{
	if (!mTransactionMap || !mAccountStateMap) throw SHAMapException(InvalidMap);
	mTransactionMap->setSynching();
	mAccountStateMap->setSynching();
}

bool Ledger::isAcquiring(void)
{
	return isAcquiringTx() || isAcquiringAS();
}

bool Ledger::isAcquiringTx(void)
{
	return mTransactionMap->isSynching();
}

bool Ledger::isAcquiringAS(void)
{
	return mAccountStateMap->isSynching();
}

boost::posix_time::ptime Ledger::getCloseTime() const
{
	return ptFromSeconds(mCloseTime);
}

void Ledger::setCloseTime(boost::posix_time::ptime ptm)
{
	mCloseTime = iToSeconds(ptm);
}

uint64 Ledger::getNextLedgerClose() const
{
	if (mCloseTime == 0)
	{
		uint64 closeTime = theApp->getOPs().getNetworkTimeNC() + mLedgerInterval - 1;
		return closeTime - (closeTime % mLedgerInterval);
	}
	return mCloseTime + mLedgerInterval;
}

// vim:ts=4
