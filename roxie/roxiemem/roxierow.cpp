/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "jexcept.hpp"
#include "jcrc.hpp"

#include "thorcommon.ipp" // for CachedOutputMetaData

#include "roxierow.hpp"

//Classes can be used to configure the allocator, and add extra data to the end.
//The checking needs to be done by setting a bit in the allocatorid
class NoCheckingHelper
{
public:
    enum {
        extraSize = 0,
        allocatorCheckFlag = 0x00000000
    };
    static inline void setCheck(size32_t size, void * ptr) {}
    static inline bool isValid(const void * ptr) { return true; }
};

//NOTE: If a row requires checking then the row will also have the bit set to indicate it requires a destructor
//so that rows are checked on destructon.
//Therefore checking if the destructor is set for a row in isValid() to protect us from uninitialised crcs.
class Crc16CheckingHelper
{
public:
    enum {
        extraSize = sizeof(unsigned short),
        allocatorCheckFlag = 0x00100000|ACTIVITY_FLAG_NEEDSDESTRUCTOR
    };
    static inline void setCheck(size32_t size, void * _ptr)
    {
        byte * ptr = static_cast<byte *>(_ptr);
        memsize_t capacity = RoxieRowCapacity(ptr);
        memset(ptr+size, 0, capacity - size - extraSize);
        unsigned short * check = reinterpret_cast<unsigned short *>(ptr + capacity - extraSize);
        *check = crc16(ptr, capacity-extraSize, 0);
    }
    static inline bool isValid(const void * _ptr)
    {
        if (RoxieRowHasDestructor(_ptr))
        {
            const byte * ptr = static_cast<const byte *>(_ptr);
            memsize_t capacity = RoxieRowCapacity(ptr);
            const unsigned short * check = reinterpret_cast<const unsigned short *>(ptr + capacity - extraSize);
            return *check == crc16(ptr, capacity-extraSize, 0);
        }
        return true;
    }
};

//This is here as demonstration of an alternative implementation...  crc16 is possibly a bit expensive.
class Sum16CheckingHelper
{
public:
    enum {
        extraSize = sizeof(unsigned short),
        allocatorCheckFlag = 0x00200000|ACTIVITY_FLAG_NEEDSDESTRUCTOR
    };
    static inline void setCheck(size32_t size, void * _ptr)
    {
        byte * ptr = static_cast<byte *>(_ptr);
        memsize_t capacity = RoxieRowCapacity(ptr);
        memset(ptr+size, 0, capacity - size - extraSize);
        unsigned short * check = reinterpret_cast<unsigned short *>(ptr + capacity - extraSize);
        *check = chksum16(ptr, capacity-extraSize);
    }
    static inline bool isValid(const void * _ptr)
    {
        if (RoxieRowHasDestructor(_ptr))
        {
            const byte * ptr = static_cast<const byte *>(_ptr);
            memsize_t capacity = RoxieRowCapacity(ptr);
            const unsigned short * check = reinterpret_cast<const unsigned short *>(ptr + capacity - extraSize);
            return chksum16(ptr, capacity-extraSize) == *check;
        }
        return true;
    }
};

bool isRowCheckValid(unsigned allocatorId, const void * row)
{
    switch (allocatorId & ALLOCATORID_CHECK_MASK)
    {
    case NoCheckingHelper::allocatorCheckFlag & ALLOCATORID_CHECK_MASK:
        return true;
    case Crc16CheckingHelper::allocatorCheckFlag & ALLOCATORID_CHECK_MASK:
        return Crc16CheckingHelper::isValid(row);
    case Sum16CheckingHelper::allocatorCheckFlag & ALLOCATORID_CHECK_MASK:
        return Sum16CheckingHelper::isValid(row);
    default:
        UNIMPLEMENTED;
    }
}

//--------------------------------------------------------------------------------------------------------------------

//More: Function to calculate the total size of a row - requires access to a rowallocator.

//--------------------------------------------------------------------------------------------------------------------
class RoxieEngineRowAllocatorBase : public CInterface, implements IEngineRowAllocator
{
public:
    RoxieEngineRowAllocatorBase(roxiemem::IRowManager & _rowManager, IOutputMetaData * _meta, unsigned _activityId, unsigned _allocatorId)
        : rowManager(_rowManager), meta(_meta)
    {
        activityId = _activityId;
        allocatorId = _allocatorId;
    }

    IMPLEMENT_IINTERFACE

//interface IEngineRowsetAllocator
    virtual byte * * createRowset(unsigned count)
    {
        if (count == 0)
            return NULL;
        return (byte **) rowManager.allocate(count * sizeof(void *), allocatorId | ACTIVITY_FLAG_ISREGISTERED);
    }

    virtual void releaseRowset(unsigned count, byte * * rowset)
    {
        rtlReleaseRowset(count, rowset);
    }

    virtual byte * * linkRowset(byte * * rowset)
    {
        return rtlLinkRowset(rowset);
    }

    virtual byte * * appendRowOwn(byte * * rowset, unsigned newRowCount, void * row)
    {
        byte * * expanded = doReallocRows(rowset, newRowCount-1, newRowCount);
        expanded[newRowCount-1] = (byte *)row;
        return expanded;
    }

    virtual byte * * reallocRows(byte * * rowset, unsigned oldRowCount, unsigned newRowCount)
    {
        //New rows (if any) aren't cleared....
        return doReallocRows(rowset, oldRowCount, newRowCount);
    }

    virtual void releaseRow(const void * row)
    {
        ReleaseRoxieRow(row);
    }

    virtual void * linkRow(const void * row)
    {
        LinkRoxieRow(row);
        return const_cast<void *>(row);
    }

    virtual IOutputMetaData * queryOutputMeta()
    {
        return meta.queryOriginal();
    }
    virtual unsigned queryActivityId()
    {
        return activityId;
    }
    virtual StringBuffer &getId(StringBuffer &idStr)
    {
        return idStr.append(activityId); // MORE - may want more context info in here
    }
    virtual IOutputRowSerializer *createDiskSerializer(ICodeContext *ctx)
    {
        return meta.createDiskSerializer(ctx, activityId);
    }
    virtual IOutputRowDeserializer *createDiskDeserializer(ICodeContext *ctx)
    {
        return meta.createDiskDeserializer(ctx, activityId);
    }
    virtual IOutputRowSerializer *createInternalSerializer(ICodeContext *ctx)
    {
        return meta.createInternalSerializer(ctx, activityId);
    }
    virtual IOutputRowDeserializer *createInternalDeserializer(ICodeContext *ctx)
    {
        return meta.createInternalDeserializer(ctx, activityId);
    }

protected:
    inline byte * * doReallocRows(byte * * rowset, unsigned oldRowCount, unsigned newRowCount)
    {
        if (!rowset)
            return createRowset(newRowCount);

        //Occasionally (in aggregates) we may try and append to a shared rowset.  In this case we need to clone the
        //target rowset.  It could be that the rowset is unshared immediately, but that is inefficient at worst.
        if (RoxieRowIsShared(rowset))
        {
            byte * * newset = createRowset(newRowCount);
            for (unsigned i=0; i < oldRowCount; i++)
            {
                byte * cur = rowset[i];
                LinkRoxieRow(cur);
                newset[i] = cur;
            }
            ReleaseRoxieRow(rowset);
            return newset;
        }

        //This would be more efficient if previous capacity was stored by the caller - or if capacity() is more efficient
        if (newRowCount * sizeof(void *) <= RoxieRowCapacity(rowset))
            return rowset;

        memsize_t capacity;
        void * ptr = (void *)rowset;
        rowManager.resizeRow(capacity, ptr, oldRowCount * sizeof(void *), newRowCount * sizeof(void *), allocatorId | ACTIVITY_FLAG_ISREGISTERED);
        return (byte * *)ptr;
    }

protected:
    roxiemem::IRowManager & rowManager;
    const CachedOutputMetaData meta;
    unsigned activityId;
    unsigned allocatorId;
};

template <class CHECKER>
class RoxieEngineFixedRowAllocator : public RoxieEngineRowAllocatorBase
{
public:
    RoxieEngineFixedRowAllocator(roxiemem::IRowManager & _rowManager, IOutputMetaData * _meta, unsigned _activityId, unsigned _allocatorId, roxiemem::RoxieHeapFlags _flags)
        : RoxieEngineRowAllocatorBase(_rowManager, _meta, _activityId, _allocatorId)
    {
        unsigned flags = _flags;
        if (meta.needsDestruct() || CHECKER::allocatorCheckFlag)
            flags |= roxiemem::RHFhasdestructor;
        heap.setown(rowManager.createFixedRowHeap(meta.getFixedSize()+CHECKER::extraSize, allocatorId | ACTIVITY_FLAG_ISREGISTERED | CHECKER::allocatorCheckFlag, (roxiemem::RoxieHeapFlags)flags));
    }

    virtual void * createRow()
    {
        return heap->allocate();
    }

    virtual void * createRow(size32_t & allocatedSize)
    {
        allocatedSize = meta.getFixedSize();
        return heap->allocate();
    }

    virtual void * resizeRow(size32_t newSize, void * row, size32_t & size)
    {
        throwUnexpected();
        return NULL;
    }

    virtual void * finalizeRow(size32_t finalSize, void * row, size32_t oldSize)
    {
        if (!meta.needsDestruct() && !CHECKER::allocatorCheckFlag)
            return row;
        CHECKER::setCheck(finalSize, row);
        return heap->finalizeRow(row);
    }

protected:
    Owned<roxiemem::IFixedRowHeap> heap;
};

template <class CHECKER>
class RoxieEngineVariableRowAllocator : public RoxieEngineRowAllocatorBase
{
public:
    RoxieEngineVariableRowAllocator(roxiemem::IRowManager & _rowManager, IOutputMetaData * _meta, unsigned _activityId, unsigned _allocatorId, roxiemem::RoxieHeapFlags _flags)
        : RoxieEngineRowAllocatorBase(_rowManager, _meta, _activityId, _allocatorId)
    {
        unsigned flags = _flags;
        if (meta.needsDestruct() || CHECKER::allocatorCheckFlag)
            flags |= roxiemem::RHFhasdestructor;
        heap.setown(rowManager.createVariableRowHeap(allocatorId | ACTIVITY_FLAG_ISREGISTERED | CHECKER::allocatorCheckFlag, (roxiemem::RoxieHeapFlags)flags));
    }

    virtual void * createRow()
    {
        memsize_t allocSize = meta.getInitialSize();
        memsize_t capacity;
        return heap->allocate(allocSize+CHECKER::extraSize, capacity);
    }

    virtual void * createRow(size32_t & allocatedSize)
    {
        const memsize_t allocSize = meta.getInitialSize();
        memsize_t newCapacity = allocatedSize;
        void * row = heap->allocate(allocSize+CHECKER::extraSize, newCapacity);
        //This test should get constant folded to avoid the decrement when not checked.
        if (CHECKER::extraSize)
            newCapacity -= CHECKER::extraSize;
        allocatedSize = newCapacity;
        return row;
    }

    virtual void * resizeRow(size32_t newSize, void * row, size32_t & size)
    {
        size32_t oldsize = size;  // don't need to include the extra checking bytes
        memsize_t newCapacity = size;
        void * newrow = heap->resizeRow(row, oldsize, newSize+CHECKER::extraSize, newCapacity);
        if (CHECKER::extraSize)
            newCapacity -= CHECKER::extraSize;
        size = newCapacity;
        return newrow;
    }

    virtual void * finalizeRow(size32_t finalSize, void * row, size32_t oldSize)
    {
        if (!meta.needsDestruct() && !CHECKER::allocatorCheckFlag)
            return row;
        void * newrow = heap->finalizeRow(row, oldSize, finalSize+CHECKER::extraSize);
        CHECKER::setCheck(finalSize, newrow);
        return newrow;
    }

protected:
    Owned<roxiemem::IVariableRowHeap> heap;
};


IEngineRowAllocator * createRoxieRowAllocator(roxiemem::IRowManager & rowManager, IOutputMetaData * meta, unsigned activityId, unsigned allocatorId, roxiemem::RoxieHeapFlags flags)
{
    if (meta->getFixedSize() != 0)
        return new RoxieEngineFixedRowAllocator<NoCheckingHelper>(rowManager, meta, activityId, allocatorId, flags);
    else
        return new RoxieEngineVariableRowAllocator<NoCheckingHelper>(rowManager, meta, activityId, allocatorId, flags);
}

IEngineRowAllocator * createCrcRoxieRowAllocator(roxiemem::IRowManager & rowManager, IOutputMetaData * meta, unsigned activityId, unsigned allocatorId, roxiemem::RoxieHeapFlags flags)
{
    if (meta->getFixedSize() != 0)
        return new RoxieEngineFixedRowAllocator<Crc16CheckingHelper>(rowManager, meta, activityId, allocatorId, flags);
    else
        return new RoxieEngineVariableRowAllocator<Crc16CheckingHelper>(rowManager, meta, activityId, allocatorId, flags);
}

#pragma pack(push,1) // hashing on members, so ensure contiguous
struct AllocatorKey
{
    IOutputMetaData *meta;
    unsigned activityId;
    AllocatorKey(IOutputMetaData *_meta, unsigned &_activityId) : meta(_meta), activityId(_activityId) { }
    bool operator==(AllocatorKey const &other) const
    {
        return (meta == other.meta) && (activityId == other.activityId);
    }
};
#pragma pack(pop)

class CAllocatorCacheItem : public OwningHTMapping<IEngineRowAllocator, AllocatorKey>
{
    Linked<IOutputMetaData> meta;
public:
    CAllocatorCacheItem(IEngineRowAllocator *allocator, AllocatorKey &key)
        : OwningHTMapping<IEngineRowAllocator, AllocatorKey>(*allocator, key)
    {
        meta.set(key.meta);
    }
};

class CAllocatorCache : public CSimpleInterface, implements IRowAllocatorMetaActIdCache
{
    OwningSimpleHashTableOf<CAllocatorCacheItem, AllocatorKey> cache;
    IArrayOf<IEngineRowAllocator> allAllocators;
    mutable SpinLock allAllocatorsLock;
    Owned<roxiemem::IRowManager> rowManager;
    roxiemem::RoxieHeapFlags flags;
    IRowAllocatorMetaActIdCacheCallback *callback;

    inline IEngineRowAllocator *_lookup(IOutputMetaData *meta, unsigned activityId) const
    {
        AllocatorKey key(meta, activityId);
        CAllocatorCacheItem *container = cache.find(key);
        if (!container)
            return NULL;
        return &container->queryElement();
    }
    inline bool _add(IEngineRowAllocator *allocator, IOutputMetaData *meta, unsigned activityId)
    {
        AllocatorKey key(meta, activityId);
        CAllocatorCacheItem *container = new CAllocatorCacheItem(allocator, key);
        return cache.replace(*container);
    }
public:
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

    CAllocatorCache(IRowAllocatorMetaActIdCacheCallback *_callback) : callback(_callback)
    {
    }
// IRowAllocatorMetaActIdCache
    inline IEngineRowAllocator *lookup(IOutputMetaData *meta, unsigned activityId) const
    {
        SpinBlock b(allAllocatorsLock);
        return _lookup(meta, activityId);
    }
    inline bool add(IEngineRowAllocator *allocator, IOutputMetaData *meta, unsigned activityId)
    {
        SpinBlock b(allAllocatorsLock);
        return _add(allocator, meta, activityId);
    }
    virtual IEngineRowAllocator *ensure(IOutputMetaData * meta, unsigned activityId)
    {
        SpinBlock b(allAllocatorsLock);
        IEngineRowAllocator *ret = _lookup(meta, activityId);
        if (ret)
            return LINK(ret);
        assertex(allAllocators.ordinality() < ALLOCATORID_MASK);
        ret = callback->createAllocator(meta, activityId, allAllocators.ordinality());
        _add(LINK(ret), meta, activityId);
        allAllocators.append(*LINK(ret));
        return ret;
    }
    virtual bool remove(IOutputMetaData *meta, unsigned activityId)
    {
        SpinBlock b(allAllocatorsLock);
        AllocatorKey key(meta, activityId);
        return cache.remove(&key);
    }
    virtual void clear()
    {
        SpinBlock b(allAllocatorsLock);
        cache.kill();
        allAllocators.kill();
    }
    virtual unsigned items() const
    {
        return allAllocators.ordinality();
    }
// roxiemem::IRowAllocatorCache
    virtual unsigned getActivityId(unsigned cacheId) const
    {
        unsigned allocatorIndex = (cacheId & ALLOCATORID_MASK);
        SpinBlock b(allAllocatorsLock);
        if (allAllocators.isItem(allocatorIndex))
            return allAllocators.item(allocatorIndex).queryActivityId();
        else
        {
            //assert(false);
            return 12345678; // Used for tracing, better than a crash...
        }
    }
    virtual StringBuffer &getActivityDescriptor(unsigned cacheId, StringBuffer &out) const
    {
        unsigned allocatorIndex = (cacheId & ALLOCATORID_MASK);
        SpinBlock b(allAllocatorsLock);
        if (allAllocators.isItem(allocatorIndex))
            return allAllocators.item(allocatorIndex).getId(out);
        else
        {
            assert(false);
            return out.append("unknown"); // Used for tracing, better than a crash...
        }
    }
    virtual void onDestroy(unsigned cacheId, void *row) const
    {
        IEngineRowAllocator *allocator;
        unsigned allocatorIndex = (cacheId & ALLOCATORID_MASK);
        {
            SpinBlock b(allAllocatorsLock); // just protect the access to the array - don't keep locked for the call of destruct or may deadlock
            if (allAllocators.isItem(allocatorIndex))
                allocator = &allAllocators.item(allocatorIndex);
            else
            {
                assert(false);
                return;
            }
        }
        if (!RoxieRowCheckValid(cacheId, row))
        {
            //MORE: Give an error, but don't throw an exception!
        }
        allocator->queryOutputMeta()->destruct((byte *) row);
    }
    virtual void checkValid(unsigned cacheId, const void *row) const
    {
        if (!RoxieRowCheckValid(cacheId, row))
        {
            //MORE: Throw an exception?
        }
    }
};

IRowAllocatorMetaActIdCache *createRowAllocatorCache(IRowAllocatorMetaActIdCacheCallback *callback)
{
    return new CAllocatorCache(callback);
}



#ifdef _USE_CPPUNIT
#include "unittests.hpp"

namespace roxiemem {

class RoxieRowAllocatorTests : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE( RoxieRowAllocatorTests );
        CPPUNIT_TEST(testSetup);
        CPPUNIT_TEST(testChecking);
        CPPUNIT_TEST(testCleanup);
        CPPUNIT_TEST(testAllocatorCache);
    CPPUNIT_TEST_SUITE_END();
    const IContextLogger &logctx;

public:
    RoxieRowAllocatorTests() : logctx(queryDummyContextLogger())
    {
    }

    ~RoxieRowAllocatorTests()
    {
    }

protected:
    class CheckingRowAllocatorCache : public CSimpleInterface, public IRowAllocatorCache
    {
    public:
        IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

        CheckingRowAllocatorCache() { numFailures = 0; }
        virtual unsigned getActivityId(unsigned cacheId) const { return 0; }
        virtual StringBuffer &getActivityDescriptor(unsigned cacheId, StringBuffer &out) const { return out.append(cacheId); }
        virtual void onDestroy(unsigned cacheId, void *row) const
        {
            if (!RoxieRowCheckValid(cacheId, row))
                ++numFailures;
        }
        virtual void checkValid(unsigned cacheId, const void *row) const
        {
            if (!RoxieRowCheckValid(cacheId, row))
                ++numFailures;
        }

        mutable unsigned numFailures;
    };

    class DummyOutputMeta : public IOutputMetaData, public CInterface
    {
    public:
        DummyOutputMeta(size32_t _minSize, size32_t _fixedSize) : minSize(_minSize), fixedSize(_fixedSize) {}
        IMPLEMENT_IINTERFACE

        virtual size32_t getRecordSize(const void *rec) { return minSize; }
        virtual size32_t getFixedSize() const { return fixedSize; }
        virtual size32_t getMinRecordSize() const { return minSize; }
        virtual void toXML(const byte * self, IXmlWriter & out) {}
        virtual unsigned getVersion() const { return 0; }
        virtual unsigned getMetaFlags() { return 0; }
        virtual IOutputMetaData * querySerializedDiskMeta() { return this; }

        virtual void destruct(byte * self) {}
        virtual IOutputRowSerializer * createDiskSerializer(ICodeContext * ctx, unsigned activityId) { return NULL; }
        virtual IOutputRowDeserializer * createDiskDeserializer(ICodeContext * ctx, unsigned activityId) { return NULL; }
        virtual ISourceRowPrefetcher * createDiskPrefetcher(ICodeContext * ctx, unsigned activityId) { return NULL; }
        virtual IOutputRowSerializer * createInternalSerializer(ICodeContext * ctx, unsigned activityId) { return NULL; }
        virtual IOutputRowDeserializer * createInternalDeserializer(ICodeContext * ctx, unsigned activityId) { return NULL; }
        virtual void walkIndirectMembers(const byte * self, IIndirectMemberVisitor & visitor) {}

        size32_t minSize;
        size32_t fixedSize;
    };

    void testAllocator(IOutputMetaData * meta, roxiemem::RoxieHeapFlags flags, unsigned low, unsigned high, int modify, bool checking)
    {
        CheckingRowAllocatorCache cache;
        Owned<IRowManager> rm = createRowManager(0, NULL, logctx, &cache);
        Owned<IEngineRowAllocator> alloc = checking ? createCrcRoxieRowAllocator(*rm, meta, 0, 0, flags) : createRoxieRowAllocator(*rm, meta, 0, 0, flags);

        for (unsigned size=low; size <= high; size++)
        {
            unsigned capacity;
            unsigned prevFailures = cache.numFailures;
            void * row = alloc->createRow(capacity);
            if (low != high)
                row = alloc->resizeRow(size, row, capacity);
            for (unsigned i1=0; i1 < capacity; i1++)
                ((byte *)row)[i1] = i1;
            const void * final = alloc->finalizeRow(capacity, row, capacity);
            for (unsigned i2=0; i2 < capacity; i2++)
            {
                ASSERT(((byte *)row)[i2] == i2);
            }

            if (modify != 0)
            {
                if (modify < 0)
                    ((byte *)row)[0]++;
                else
                    ((byte *)row)[size-1]++;
            }
            ReleaseRoxieRow(row);
            if (modify == 0)
            {
                ASSERT(prevFailures == cache.numFailures);
            }
            else
            {
                ASSERT(prevFailures+1 == cache.numFailures);
            }
        }
    }

    void testAllocator(IOutputMetaData * meta, roxiemem::RoxieHeapFlags flags, unsigned low, unsigned high)
    {
        testAllocator(meta, flags, low, high, 0, false);
        testAllocator(meta, flags, low, high, 0, true);
        testAllocator(meta, flags, low, high, -1, true);
        testAllocator(meta, flags, low, high, +1, true);
    }

    void testSetup()
    {
        setTotalMemoryLimit(40*HEAP_ALIGNMENT_SIZE, 0, NULL);
    }

    void testCleanup()
    {
        releaseRoxieHeap();
    }

    void testChecking()
    {
        Owned<IRowManager> rm = createRowManager(0, NULL, logctx, NULL);

        for (unsigned fixedSize=1; fixedSize<64; fixedSize++)
        {
            DummyOutputMeta meta(fixedSize, fixedSize);
            testAllocator(&meta, RHFnone, fixedSize, fixedSize);
            testAllocator(&meta, RHFpacked, fixedSize, fixedSize);
        }

        for (unsigned varSize=1; varSize<64; varSize++)
        {
            DummyOutputMeta meta(varSize, 0);
            testAllocator(&meta, RHFnone, varSize, varSize);
            testAllocator(&meta, RHFnone, 1, varSize);
        }
    }

    void testAllocatorCache()
    {
        IArrayOf<IOutputMetaData> metas;
        Owned<IRowManager> rm = createRowManager(0, NULL, logctx, NULL);
        class CAllocatorCallback : implements IRowAllocatorMetaActIdCacheCallback
        {
            IRowManager *rm;
        public:
            CAllocatorCallback(IRowManager *_rm) : rm(_rm)
            {
            }
            virtual IEngineRowAllocator *createAllocator(IOutputMetaData *meta, unsigned activityId, unsigned cacheId) const
            {
                return createRoxieRowAllocator(*rm, meta, activityId, cacheId, roxiemem::RHFnone);
            }
        } callback(rm);
        Owned<IRowAllocatorMetaActIdCache> allocatorCache = createRowAllocatorCache(&callback);
        for (unsigned fixedSize=1; fixedSize<=64; fixedSize++)
        {
            DummyOutputMeta *meta = new DummyOutputMeta(fixedSize, fixedSize);
            metas.append(*meta);

            unsigned activityId = 1 + ((fixedSize-1) % 32); // i.e. make an id, so half are duplicates
            Owned<IEngineRowAllocator> allocator = allocatorCache->ensure(meta, activityId);
        }
        ASSERT(allocatorCache->items() == 64);
        Owned<IEngineRowAllocator> allocator = allocatorCache->lookup(&metas.item(0), 1);
        ASSERT(NULL != allocator);
        ASSERT(allocatorCache->remove(&metas.item(0), 1));
        ASSERT(allocatorCache->add(allocator.getClear(), &metas.item(0), 1));

        for (unsigned fixedSize=1; fixedSize<=64; fixedSize++)
        {
            unsigned activityId = 1 + ((fixedSize-1) % 32); // i.e. make an id, so half are duplicates
            IOutputMetaData *meta = &metas.item(fixedSize-1); // from 1st round
            Owned<IEngineRowAllocator> allocator = allocatorCache->ensure(meta, activityId);
        }
        ASSERT(allocatorCache->items() == 64);

        metas.kill();
        allocatorCache.clear();
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION( RoxieRowAllocatorTests );
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION( RoxieRowAllocatorTests, "RoxieRowAllocatorTests" );

} // namespace roxiemem
#endif
