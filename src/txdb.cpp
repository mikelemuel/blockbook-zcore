// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2016-2018 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"

#include "main.h"
#include "pow.h"
#include "uint256.h"
#include "accumulators.h"

#include <stdint.h>

#include <boost/thread.hpp>

using namespace std;
using namespace libzerocoin;


static const char DB_ADDRESSUNSPENTINDEX = 'u';
static const char DB_TIMESTAMPINDEX = 's';
static const char DB_ADDRESSINDEX = 'a';
static const char DB_SPENTINDEX = 'p';

void static BatchWriteCoins(CDBBatch& batch, const uint256& hash, const CCoins& coins)
{
    if (coins.IsPruned())
        batch.Erase(make_pair('c', hash));
    else
        batch.Write(make_pair('c', hash), coins);
}

void static BatchWriteHashBestChain(CDBBatch& batch, const uint256& hash)
{
    batch.Write('B', hash);
}

CCoinsViewDB::CCoinsViewDB(size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / "chainstate", nCacheSize, fMemory, fWipe)
{
}

bool CCoinsViewDB::GetCoins(const uint256& txid, CCoins& coins) const
{
    return db.Read(make_pair('c', txid), coins);
}

bool CCoinsViewDB::HaveCoins(const uint256& txid) const
{
    return db.Exists(make_pair('c', txid));
}

uint256 CCoinsViewDB::GetBestBlock() const
{
    uint256 hashBestChain;
    if (!db.Read('B', hashBestChain))
        return uint256(0);
    return hashBestChain;
}

bool CCoinsViewDB::BatchWrite(CCoinsMap& mapCoins, const uint256& hashBlock)
{
    CDBBatch batch(&db.GetObfuscateKey());
    size_t count = 0;
    size_t changed = 0;
    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end();) {
        if (it->second.flags & CCoinsCacheEntry::DIRTY) {
            BatchWriteCoins(batch, it->first, it->second.coins);
            changed++;
        }
        count++;
        CCoinsMap::iterator itOld = it++;
        mapCoins.erase(itOld);
    }
    if (hashBlock != uint256(0))
        BatchWriteHashBestChain(batch, hashBlock);

    LogPrint("coindb", "Committing %u changed transactions (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    return db.WriteBatch(batch);
}

CBlockTreeDB::CBlockTreeDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "blocks" / "index", nCacheSize, fMemory, fWipe)
{
}

bool CBlockTreeDB::WriteBlockIndex(const CDiskBlockIndex& blockindex)
{
    return Write(make_pair('b', blockindex.GetBlockHash()), blockindex);
}

bool CBlockTreeDB::WriteBlockFileInfo(int nFile, const CBlockFileInfo& info)
{
    return Write(make_pair('f', nFile), info);
}

bool CBlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo& info)
{
    return Read(make_pair('f', nFile), info);
}

bool CBlockTreeDB::WriteLastBlockFile(int nFile)
{
    return Write('l', nFile);
}

bool CBlockTreeDB::WriteReindexing(bool fReindexing)
{
    if (fReindexing)
        return Write('R', '1');
    else
        return Erase('R');
}

bool CBlockTreeDB::ReadReindexing(bool& fReindexing)
{
    fReindexing = Exists('R');
    return true;
}

bool CBlockTreeDB::ReadLastBlockFile(int& nFile)
{
    return Read('l', nFile);
}

bool CCoinsViewDB::GetStats(CCoinsStats& stats) const
{
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&db)->NewIterator());
    pcursor->SeekToFirst();

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    stats.hashBlock = GetBestBlock();
    ss << stats.hashBlock;
    CAmount nTotalAmount = 0;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        try {
            leveldb::Slice slKey = pcursor->key();
            CDataStream ssKey(slKey.data(), slKey.data() + slKey.size(), SER_DISK, CLIENT_VERSION);
            char chType;
            ssKey >> chType;
            if (chType == 'c') {
                leveldb::Slice slValue = pcursor->value();
                CDataStream ssValue(slValue.data(), slValue.data() + slValue.size(), SER_DISK, CLIENT_VERSION);
                CCoins coins;
                ssValue >> coins;
                uint256 txhash;
                ssKey >> txhash;
                ss << txhash;
                ss << VARINT(coins.nVersion);
                ss << (coins.fCoinBase ? 'c' : 'n');
                ss << VARINT(coins.nHeight);
                stats.nTransactions++;
                for (unsigned int i = 0; i < coins.vout.size(); i++) {
                    const CTxOut& out = coins.vout[i];
                    if (!out.IsNull()) {
                        stats.nTransactionOutputs++;
                        ss << VARINT(i + 1);
                        ss << out;
                        nTotalAmount += out.nValue;
                    }
                }
                stats.nSerializedSize += 32 + slValue.size();
                ss << VARINT(0);
            }
            pcursor->Next();
        } catch (std::exception& e) {
            return error("%s : Deserialize or I/O error - %s", __func__, e.what());
        }
    }
    stats.nHeight = mapBlockIndex.find(GetBestBlock())->second->nHeight;
    stats.hashSerialized = ss.GetHash();
    stats.nTotalAmount = nTotalAmount;
    return true;
}

bool CBlockTreeDB::ReadTxIndex(const uint256& txid, CDiskTxPos& pos)
{
    return Read(make_pair('t', txid), pos);
}

bool CBlockTreeDB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >& vect)
{
    CDBBatch batch(&GetObfuscateKey());
    for (std::vector<std::pair<uint256, CDiskTxPos> >::const_iterator it = vect.begin(); it != vect.end(); it++)
        batch.Write(make_pair('t', it->first), it->second);
    return WriteBatch(batch);
}

bool CBlockTreeDB::WriteFlag(const std::string& name, bool fValue)
{
    return Write(std::make_pair('F', name), fValue ? '1' : '0');
}

bool CBlockTreeDB::ReadFlag(const std::string& name, bool& fValue)
{
    char ch;
    if (!Read(std::make_pair('F', name), ch))
        return false;
    fValue = ch == '1';
    return true;
}

bool CBlockTreeDB::WriteInt(const std::string& name, int nValue)
{
    return Write(std::make_pair('I', name), nValue);
}

bool CBlockTreeDB::ReadInt(const std::string& name, int& nValue)
{
    return Read(std::make_pair('I', name), nValue);
}

bool CBlockTreeDB::LoadBlockIndexGuts()
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    CDataStream ssKeySet(SER_DISK, CLIENT_VERSION);
    ssKeySet << make_pair('b', uint256(0));
    pcursor->Seek(ssKeySet.str());

    // Load mapBlockIndex
    uint256 nPreviousCheckpoint;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        try {
            leveldb::Slice slKey = pcursor->key();
            CDataStream ssKey(slKey.data(), slKey.data() + slKey.size(), SER_DISK, CLIENT_VERSION);
            char chType;
            ssKey >> chType;
            if (chType == 'b') {
                leveldb::Slice slValue = pcursor->value();
                CDataStream ssValue(slValue.data(), slValue.data() + slValue.size(), SER_DISK, CLIENT_VERSION);
                CDiskBlockIndex diskindex;
                ssValue >> diskindex;

                // Construct block index object
                CBlockIndex* pindexNew = InsertBlockIndex(diskindex.GetBlockHash());
                pindexNew->pprev = InsertBlockIndex(diskindex.hashPrev);
                pindexNew->pnext = InsertBlockIndex(diskindex.hashNext);
                pindexNew->nHeight = diskindex.nHeight;
                pindexNew->nFile = diskindex.nFile;
                pindexNew->nDataPos = diskindex.nDataPos;
                pindexNew->nUndoPos = diskindex.nUndoPos;
                pindexNew->nVersion = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime = diskindex.nTime;
                pindexNew->nBits = diskindex.nBits;
                pindexNew->nNonce = diskindex.nNonce;
                pindexNew->nStatus = diskindex.nStatus;
                pindexNew->nTx = diskindex.nTx;

                //zerocoin
                pindexNew->nAccumulatorCheckpoint = diskindex.nAccumulatorCheckpoint;
                pindexNew->mapZerocoinSupply = diskindex.mapZerocoinSupply;
                pindexNew->vMintDenominationsInBlock = diskindex.vMintDenominationsInBlock;

                //Proof Of Stake
                pindexNew->nMint = diskindex.nMint;
                pindexNew->nMoneySupply = diskindex.nMoneySupply;
                pindexNew->nFlags = diskindex.nFlags;
                pindexNew->nStakeModifier = diskindex.nStakeModifier;
                pindexNew->prevoutStake = diskindex.prevoutStake;
                pindexNew->nStakeTime = diskindex.nStakeTime;
                pindexNew->hashProofOfStake = diskindex.hashProofOfStake;

                if (pindexNew->nHeight <= Params().LAST_POW_BLOCK()) {
                    if (!CheckProofOfWork(pindexNew->GetBlockHash(), pindexNew->nBits))
                        return error("LoadBlockIndex() : CheckProofOfWork failed: %s", pindexNew->ToString());
                }
                // ppcoin: build setStakeSeen
                if (pindexNew->IsProofOfStake())
                    setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));

                //populate accumulator checksum map in memory
                if(pindexNew->nAccumulatorCheckpoint != 0 && pindexNew->nAccumulatorCheckpoint != nPreviousCheckpoint) {
                    //Don't load any checkpoints that exist before v2 zzcr. The accumulator is invalid for v1 and not used.
                    if (pindexNew->nHeight >= Params().Zerocoin_Block_V2_Start())
                        LoadAccumulatorValuesFromDB(pindexNew->nAccumulatorCheckpoint);

                    nPreviousCheckpoint = pindexNew->nAccumulatorCheckpoint;
                }

                pcursor->Next();
            } else {
                break; // if shutdown requested or finished loading block index
            }
        } catch (std::exception& e) {
            return error("%s : Deserialize or I/O error - %s", __func__, e.what());
        }
    }

    return true;
}


bool CBlockTreeDB::UpdateAddressUnspentIndex(const std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue > >&vect) {
    CDBBatch batch(&GetObfuscateKey());
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it=vect.begin(); it!=vect.end(); it++) {
        if (it->second.IsNull()) {
            batch.Erase(make_pair(DB_ADDRESSUNSPENTINDEX, it->first));
        } else {
            batch.Write(make_pair(DB_ADDRESSUNSPENTINDEX, it->first), it->second);
        }
    }
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadAddressUnspentIndex(uint160 addressHash, int type,
                                           std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > &unspentOutputs) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
    pcursor->Seek(make_pair(DB_ADDRESSUNSPENTINDEX, CAddressIndexIteratorKey(type, addressHash)));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char,CAddressUnspentKey> key;
        if (pcursor->GetKey(key) && key.first == DB_ADDRESSUNSPENTINDEX && key.second.hashBytes == addressHash) {
            CAddressUnspentValue nValue;
            if (pcursor->GetValue(nValue)) {
                unspentOutputs.push_back(make_pair(key.second, nValue));
                pcursor->Next();
            } else {
                return error("failed to get address unspent value");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CBlockTreeDB::ReadSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value) {
    return Read(make_pair(DB_SPENTINDEX, key), value);
}

bool CBlockTreeDB::UpdateSpentIndex(const std::vector<std::pair<CSpentIndexKey, CSpentIndexValue> >&vect) {
    CDBBatch batch(&GetObfuscateKey());
    for (std::vector<std::pair<CSpentIndexKey,CSpentIndexValue> >::const_iterator it=vect.begin(); it!=vect.end(); it++) {
        if (it->second.IsNull()) {
            batch.Erase(make_pair(DB_SPENTINDEX, it->first));
        } else {
            batch.Write(make_pair(DB_SPENTINDEX, it->first), it->second);
        }
    }
    return WriteBatch(batch);
}


bool CBlockTreeDB::WriteTimestampIndex(const CTimestampIndexKey &timestampIndex) {
    CDBBatch batch(&GetObfuscateKey());
    batch.Write(make_pair(DB_TIMESTAMPINDEX, timestampIndex), 0);
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadTimestampIndex(const unsigned int &high, const unsigned int &low, std::vector<uint256> &hashes) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_TIMESTAMPINDEX, CTimestampIndexIteratorKey(low)));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, CTimestampIndexKey> key;
        if (pcursor->GetKey(key) && key.first == DB_TIMESTAMPINDEX && key.second.timestamp <= high) {
            hashes.push_back(key.second.blockHash);
            pcursor->Next();
        } else {
            break;
        }
    }

    return true;
}
bool CBlockTreeDB::WriteAddressIndex(const std::vector<std::pair<CAddressIndexKey, CAmount > >&vect) {
    CDBBatch batch(&GetObfuscateKey());
    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(make_pair(DB_ADDRESSINDEX, it->first), it->second);
    return WriteBatch(batch);
}

bool CBlockTreeDB::EraseAddressIndex(const std::vector<std::pair<CAddressIndexKey, CAmount > >&vect) {
    CDBBatch batch(&GetObfuscateKey());
    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Erase(make_pair(DB_ADDRESSINDEX, it->first));
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadAddressIndex(uint160 addressHash, int type,
                                    std::vector<std::pair<CAddressIndexKey, CAmount> > &addressIndex,
                                    int start, int end) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    if (start > 0 && end > 0) {
        pcursor->Seek(make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorHeightKey(type, addressHash, start)));
    } else {
        pcursor->Seek(make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorKey(type, addressHash)));
    }

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char,CAddressIndexKey> key;
        if (pcursor->GetKey(key) && key.first == DB_ADDRESSINDEX && key.second.hashBytes == addressHash) {
            if (end > 0 && key.second.blockHeight > end) {
                break;
            }
            CAmount nValue;
            if (pcursor->GetValue(nValue)) {
                addressIndex.push_back(make_pair(key.second, nValue));
                pcursor->Next();
            } else {
                return error("failed to get address index value");
            }
        } else {
            break;
        }
    }

    return true;
}


CZerocoinDB::CZerocoinDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "zerocoin", nCacheSize, fMemory, fWipe)
{
}

bool CZerocoinDB::WriteCoinMintBatch(const std::vector<std::pair<libzerocoin::PublicCoin, uint256> >& mintInfo)
{
    CDBBatch batch;
    size_t count = 0;
    for (std::vector<std::pair<libzerocoin::PublicCoin, uint256> >::const_iterator it=mintInfo.begin(); it != mintInfo.end(); it++) {
        PublicCoin pubCoin = it->first;
        uint256 hash = GetPubCoinHash(pubCoin.getValue());
        batch.Write(make_pair('m', hash), it->second);
        ++count;
    }

    LogPrint("zero", "Writing %u coin mints to db.\n", (unsigned int)count);
    return WriteBatch(batch, true);
}

bool CZerocoinDB::ReadCoinMint(const CBigNum& bnPubcoin, uint256& hashTx)
{
    return ReadCoinMint(GetPubCoinHash(bnPubcoin), hashTx);
}

bool CZerocoinDB::ReadCoinMint(const uint256& hashPubcoin, uint256& hashTx)
{
    return Read(make_pair('m', hashPubcoin), hashTx);
}

bool CZerocoinDB::EraseCoinMint(const CBigNum& bnPubcoin)
{
    uint256 hash = GetPubCoinHash(bnPubcoin);
    return Erase(make_pair('m', hash));
}

bool CZerocoinDB::WriteCoinSpendBatch(const std::vector<std::pair<libzerocoin::CoinSpend, uint256> >& spendInfo)
{
    CDBBatch batch;
    size_t count = 0;
    for (std::vector<std::pair<libzerocoin::CoinSpend, uint256> >::const_iterator it=spendInfo.begin(); it != spendInfo.end(); it++) {
        CBigNum bnSerial = it->first.getCoinSerialNumber();
        CDataStream ss(SER_GETHASH, 0);
        ss << bnSerial;
        uint256 hash = Hash(ss.begin(), ss.end());
        batch.Write(make_pair('s', hash), it->second);
        ++count;
    }

    LogPrint("zero", "Writing %u coin spends to db.\n", (unsigned int)count);
    return WriteBatch(batch, true);
}

bool CZerocoinDB::ReadCoinSpend(const CBigNum& bnSerial, uint256& txHash)
{
    CDataStream ss(SER_GETHASH, 0);
    ss << bnSerial;
    uint256 hash = Hash(ss.begin(), ss.end());

    return Read(make_pair('s', hash), txHash);
}

bool CZerocoinDB::ReadCoinSpend(const uint256& hashSerial, uint256 &txHash)
{
    return Read(make_pair('s', hashSerial), txHash);
}

bool CZerocoinDB::EraseCoinSpend(const CBigNum& bnSerial)
{
    CDataStream ss(SER_GETHASH, 0);
    ss << bnSerial;
    uint256 hash = Hash(ss.begin(), ss.end());

    return Erase(make_pair('s', hash));
}

bool CZerocoinDB::WipeCoins(std::string strType)
{
    if (strType != "spends" && strType != "mints")
        return error("%s: did not recognize type %s", __func__, strType);

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    char type = (strType == "spends" ? 's' : 'm');
    CDataStream ssKeySet(SER_DISK, CLIENT_VERSION);
    ssKeySet << make_pair(type, uint256(0));
    pcursor->Seek(ssKeySet.str());
    // Load mapBlockIndex
    std::set<uint256> setDelete;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        try {
            leveldb::Slice slKey = pcursor->key();
            CDataStream ssKey(slKey.data(), slKey.data() + slKey.size(), SER_DISK, CLIENT_VERSION);
            char chType;
            ssKey >> chType;
            if (chType == type) {
                leveldb::Slice slValue = pcursor->value();
                CDataStream ssValue(slValue.data(), slValue.data() + slValue.size(), SER_DISK, CLIENT_VERSION);
                uint256 hash;
                ssValue >> hash;
                setDelete.insert(hash);
                pcursor->Next();
            } else {
                break; // if shutdown requested or finished loading block index
            }
        } catch (std::exception& e) {
            return error("%s : Deserialize or I/O error - %s", __func__, e.what());
        }
    }

    for (auto& hash : setDelete) {
        if (!Erase(make_pair(type, hash)))
            LogPrintf("%s: error failed to delete %s\n", __func__, hash.GetHex());
    }

    return true;
}

bool CZerocoinDB::WriteAccumulatorValue(const uint32_t& nChecksum, const CBigNum& bnValue)
{
    LogPrint("zero","%s : checksum:%d val:%s\n", __func__, nChecksum, bnValue.GetHex());
    return Write(make_pair('2', nChecksum), bnValue);
}

bool CZerocoinDB::ReadAccumulatorValue(const uint32_t& nChecksum, CBigNum& bnValue)
{
    return Read(make_pair('2', nChecksum), bnValue);
}

bool CZerocoinDB::EraseAccumulatorValue(const uint32_t& nChecksum)
{
    LogPrint("zero", "%s : checksum:%d\n", __func__, nChecksum);
    return Erase(make_pair('2', nChecksum));
}
