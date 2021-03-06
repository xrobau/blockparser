
#include <util.h>
#include <common.h>
#include <errlog.h>
#include <option.h>
#include <string.h>
#include <callback.h>
#include <ctime>
#include <sys/stat.h>

#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <sqlite3.h>
    
using boost::shared_ptr;

typedef GoogMap<Hash256, uint32_t, Hash256Hasher, Hash256Equal>::Map TXTimeMap;
static uint8_t empty[kSHA256ByteSize] = { 0x42 };
static int64_t database_last_block;
    
struct sqliteSync:public Callback
{
    optparse::OptionParser parser;


    bool proofOfStake;
    bool verbose;
    bool emptyOutput;
    bool skip;
    bool drop;
    uint64_t inputValue;
    uint64_t baseReward;
    uint64_t last_pow_reward;
    uint8_t txCount;
    int blkTxCount;
    size_t nbInputs;
    bool hasGenInput;
    uint64_t currBlock;
    uint64_t blockFee;
    uint32_t bits;
    uint32_t last_pow_bits;
    uint32_t last_pos_bits;
    uint32_t nonce;
    uint32_t txTime;
    const uint8_t *prevBlkHash;
    const uint8_t *blkMerkleRoot;
    time_t time;
    const uint8_t *currTXHash;
    
    uint32_t POScount;
    uint32_t POWcount;
    uint64_t totalFeeDestroyed;
    uint64_t totalMined;
    uint64_t totalStakeEarned;
    uint32_t totalTrans;
    uint128_t totalSent;
    uint128_t totalReceived;
    uint64_t block_sent;
    uint64_t block_received;
    double stakeAge; //age of stake transaction in days

    int block_inserts;
    int block_existing;

    std::string path;
    std::string dbname;
    sqlite3 *db;
        
    TXTimeMap gTXTimeMap;


    uint8_t *blockHash;

    sqliteSync()
    {
        parser
            .usage("[options]")
            .version("")
            .description("load/sync the blockchain with a sqlite database")
            .epilog("")
        ;
        parser
            .add_option("-p", "--path")
            .dest("path")
            .set_default(".")
            .help("path to the sqlite database")
        ;
        parser
            .add_option("-n", "--name")
            .dest("name")
            .set_default("peerchain.db")
            .help("database name")
        ;
        parser
            .add_option("-v", "--verbose")
            .action("store_true")
            .set_default(false)
            .help("verbose")
         ;
        parser
            .add_option("-d", "--drop")
            .action("store_true")
            .set_default(false)
            .help("drop database and start over if it exists")
         ;

    }

    virtual const char                   *name() const         { return "sqlitesync"; }
    virtual const optparse::OptionParser *optionParser() const { return &parser;   }
    virtual bool                         needTXHash() const    { return true;      }

    virtual void aliases(
        std::vector<const char*> &v
    ) const
    {
        v.push_back("sync");
    }
    virtual bool file_exists(std::string full_path) {
        //check if path/dbname exists
        struct stat buffer;
        return( stat (full_path.c_str(), &buffer) == 0);
        
    }
/*
    virtual bool keyspace_match(std::string keyspace,cql::cql_result_t& result) {

        while(result.next()) {
            cql::cql_byte_t* data = NULL;
            cql::cql_int_t size = 0;
            result.get_data("keyspace_name",&data,size);
            char* name = reinterpret_cast<char*>(data); 
            if(verbose) {
                printf("found keyspace %s\n",name);
            }
            if(boost::equals(name,keyspace))
                return true; 

        }    
        return false;

    }
*/
    virtual int init_sqlite(std::string full_path) {
        int rc;
        rc = sqlite3_open(full_path.c_str(), &db);

        return rc;
    }
    virtual int64_t get_block_count() {
        //value is actually set in callback
        std::string query = "SELECT MAX(id) from blocks"; 
        int rc;
        char *zErrMsg = 0;
        rc = sqlite3_exec(db, query.c_str(), last_block_callback, 0, &zErrMsg);
        if( rc != SQLITE_OK ) {
            info("failed to execute query: %s",query.c_str());
            errFatal("sql error: %s", zErrMsg);  
        }
        return 0;

    }
    static int last_block_callback(void *NotUsed, int argc, char **argv, char **azColName){
        database_last_block = atoi(argv[0]);
        return 0;
    }
    //basic query callback
    static int callback(void *NotUsed, int argc, char **argv, char **azColName){
        int i;
        for(i=0; i<argc; i++){
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
        }
        printf("\n");
        return 0;
    }
    virtual bool call_query(std::string query) {
        int rc;
        char *zErrMsg = 0;
        rc = sqlite3_exec(db, query.c_str(), callback, 0, &zErrMsg);
        if( rc != SQLITE_OK ) {
            info("failed to execute query: %s",query.c_str());
            errFatal("sql error: %s", zErrMsg);  
        }
        return true;
    }
    virtual bool increment_counters() {
       uint64_t totalSupply = totalMined + totalStakeEarned - totalFeeDestroyed;
       uint64_t msTime = time*1000;
       std::string query = str(boost::format("INSERT INTO stats (time, last_block, destroyed_fees,"
        " minted_coins, mined_coins,"
        " money_supply, pos_blocks, pow_blocks, transactions, pow_block_reward, pos_difficulty, pow_difficulty) VALUES "
        "(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d)") % msTime % (int)currBlock % 
        totalFeeDestroyed % totalStakeEarned % totalMined % totalSupply % 
        POScount % POWcount % totalTrans % last_pow_reward % diff(last_pos_bits) % diff(last_pow_bits)); 
        //totalSent % totalReceived % totalTrans);
       if(verbose) printf("%s\n",query.c_str());
       call_query(query);
       return true;
    }
    
    virtual bool create_stats_table() {
        return call_query("CREATE TABLE IF NOT EXISTS stats ("
        " time timestamp,"
        " last_block int,"
        " pos_blocks int,"
        " pow_blocks int,"
        " transactions int,"
        " money_supply bigint,"
        " destroyed_fees bigint,"
        " minted_coins bigint,"
        " mined_coins bigint,"
        " pow_block_reward bigint,"
        " pos_difficulty float,"
        " pow_difficulty float,"
        " PRIMARY KEY (last_block))"
        //" sent bigint,"
        //" received bigint,"
        );

    }
    virtual bool create_block_table() {
       return call_query(
       "CREATE TABLE IF NOT EXISTS blocks ("
            "id int,"
            "chain int,"
            "stakeage float,"
            "pos boolean,"
            "hash varchar,"
            "hashPrevBlock varchar,"
            "hashMerkleRoot varchar,"
            "time timestamp,"
            "bits varchar,"
            "difficulty float,"
            "nonce bigint,"
            "txcount int,"
            "reward bigint,"
            "staked bigint,"
            "sent bigint,"
            "received bigint,"
            "destroyed bigint,"
            "PRIMARY KEY (id,chain))"
       );
    }/*
    //need to make it parse txns
    virtual bool create_tx_table() {
       return false;
       return call_query(
       "CREATE TABLE IF NOT EXISTS transactions ("
            "id int,"
            "chain int,"
            "POS boolean,"
            "hashPrevBlock varchar,"
            "hash varchar,"
            "hashMerkleRoot varchar,"
            "time timestamp,"
            "bits varchar,"
            "diff float,"
            "nonce bigint,"
            "txcount int,"
            "reward bigint,"
            "staked bigint,"
            "destroyed bigint,"
            "PRIMARY KEY (id,chain))"
            " with caching = 'all';"
       );
    }

    virtual bool block_exists() {
       if(drop) {
            return false;
       }
       shared_ptr<cql::cql_query_t> block_exists(new cql::cql_query_t(str(boost::format("SELECT id FROM blocks WHERE ID = %1%;") % (int)currBlock)));
       future = session->query(block_exists);
       future.wait();
       if(future.get().error.is_err()) {
           std::cout << boost::format("cql error: %1%") % future.get().error.message << "\n";
           errFatal("Failed to check if block exists");
       }
       cql::cql_result_t& result = *future.get().result;
       if(result.next()) {
            if(verbose) { info("block %d already exists",(int)currBlock); }
            return true;
       }
       if(verbose) { info("block %d does not exists",(int)currBlock); }
       return false;

        
    }
    */
    virtual void add_block() {

       std::string POS = proofOfStake ? "TRUE" : "FALSE";
       uint8_t *strblockHash = (uint8_t*)alloca(2*kSHA256ByteSize+1);
       //uint8_t *strblockHash = allocHash256();
       uint8_t *strblkMerkleRoot = (uint8_t*)alloca(2*kSHA256ByteSize+1);
       //uint8_t *strblkMerkleRoot = allocHash256();
       uint8_t *strprevBlkHash = (uint8_t*)alloca(2*kSHA256ByteSize+1); 
       //uint8_t *strprevBlkHash = allocHash256(); 
       //showHex(blockHash);
       //printf("\n");
       //showHex(prevBlkHash);
       //printf("\n");
       //showHex(blkMerkleRoot);
       //printf("\n");
       toHex(strblockHash, blockHash);
       toHex(strblkMerkleRoot,blkMerkleRoot);
       toHex(strprevBlkHash,prevBlkHash);
       uint64_t msTime = time*1000;
       std::string query = str(boost::format(
       "INSERT INTO blocks (id,chain,stakeage,pos,hash,hashprevblock,hashmerkleroot,time,bits,difficulty,nonce,txcount,reward,staked,sent,received,destroyed) "
       "VALUES (%d,0,%f,'%s','%s','%s','%s',%d,'%x',%f,%u,%d,%d,%d,%d,%d,%d)") % (int)currBlock % stakeAge % POS % strblockHash % strprevBlkHash % strblkMerkleRoot % msTime % bits %
            diff(bits) % nonce % blkTxCount % baseReward % inputValue % block_sent % block_received % blockFee);
       if(verbose) {
            printf("%s\n",query.c_str());
       }
       call_query(query.c_str());
       block_inserts++;
    }
    virtual int init(
        int argc,
        const char *argv[]
    )
    {
        block_inserts = 0;
        block_existing = 0;
        last_pow_bits = 0x1c00ffff; //peercoin started at diff 256
        last_pos_bits = 0x1d00ffff; //unsure of inital POS diff - guessing 1
        last_pow_reward = 0;

        optparse::Values values = parser.parse_args(argc, argv);
        path = values["path"].c_str();
        dbname = values["name"].c_str();

        drop = values.get("drop");
        verbose = values.get("verbose");

        gTXTimeMap.setEmptyKey(empty);
        gTXTimeMap.resize(15*1000*1000);

        std::string full_path = path + "/" + dbname;

        info("using sqlite database at %s",full_path.c_str());

        try {
            bool exists = file_exists(full_path);
            if(exists) {
                info("database already exists");
                if(drop) {
                    info("removing the database...");
                    int deleted = std::remove(full_path.c_str());
                    if(deleted) {
                        errFatal("failed to remove existing database");
                    }
                }
            } else {
                info("db file does not exist, creating anew");
            }
            if(init_sqlite(full_path) == 0) {
                if(drop || !exists) {
                    info("created db and connected successfully"); 
                } else 
                    info("connected successfully"); 
            } else {
                errFatal("failed to connect to db");
            }
            if(drop || !exists) {
                //create tables
                if(create_stats_table()) {
                    info("sucessfully created stats table");
                }
                if(create_block_table()) {
                    info("successfully created block table");
                 }
                //if(create_tx_table() && verbose) {
                //    info("successfully created/did not delete tx table");
                //}
            }
            if(exists && !drop) {
                // database_last_block is set in the sqlite callback
                get_block_count();
                info("found %lld existing blocks",database_last_block+1);
            } else {
                database_last_block = -1;
            }

            printf("\n");
            info("starting block insert process");
            call_query("BEGIN TRANSACTION");


        }
        catch (std::exception& e)
        {
            std::cout << "Exception: " << e.what() << std::endl;
        }

        
        POScount = 0;
        POWcount = 0;
        totalFeeDestroyed = 0;
        totalMined = 0;
        totalStakeEarned = 0;
        totalTrans = 0;
        totalSent = 0;
        totalReceived = 0;
        
        return 0;
    }

    virtual void startBlock(
        const Block *b,
        uint64_t
    )
    {
        const uint8_t *p = b->data;
        blockHash = allocHash256();
        sha256Twice(blockHash, p, 80); 
        SKIP(uint32_t, version, p);
        prevBlkHash = p;
        SKIP(uint256_t, prevhash, p);
        blkMerkleRoot = p;
        SKIP(uint256_t, merkleroot, p);
        LOAD(uint32_t, blkTime, p);
        LOAD(uint32_t, blkBits, p);
        LOAD(uint32_t, blkNonce, p);
        LOAD_VARINT(nbTX, p);
        currBlock = b->height - 1;
        bits = blkBits;
        time = blkTime;
        nonce = blkNonce;
        proofOfStake = false;
        baseReward = 0;
        inputValue = 0;
        txCount = 0;
        blkTxCount = nbTX;
        blockFee = 0;
        block_sent = 0;
        block_received = 0;
        //currblock should be equal to last_block to skip it
        if((int)currBlock <= database_last_block) {
            skip = true;
            block_existing++;
        } else 
            skip = false;
    }

    virtual void startTX(
        const uint8_t *p,
        const uint8_t *hash
    )
    {
        currTXHash = hash;
        txCount++;
        SKIP(uint32_t, version, p);
        LOAD(uint32_t, ntime, p);
        txTime = ntime;
        gTXTimeMap[hash] = ntime;
    }

    virtual void  startInputs(const uint8_t *p)
    {
        hasGenInput = false;
        emptyOutput = false;
        nbInputs = 0;
    }

    virtual void   startInput(const uint8_t *p)
    {
        static uint256_t gNullHash;
        bool isGenInput = (0==memcmp(gNullHash.v, p, sizeof(gNullHash)));
        if(isGenInput) hasGenInput = true;
        ++nbInputs;
    }

    virtual void  endInputs(const uint8_t *p)
    {
        if(hasGenInput) {
            if(1!=nbInputs) abort();
        }
    }
    virtual void edge(
        uint64_t      value,
        const uint8_t *upTXHash,
        uint64_t      outputIndex,
        const uint8_t *outputScript,
        
        uint64_t      outputScriptSize,
        const uint8_t *downTXHash,
        uint64_t      inputIndex,
        const uint8_t *inputScript,
        uint64_t      inputScriptSize) {

        if(proofOfStake && txCount == 2) {
            inputValue += value;
            stakeAge = (txTime - gTXTimeMap[upTXHash]) / (float)(60*60*24);
        } else {
            totalSent += value;
            block_sent += value;
            blockFee += value;
            //if(verbose) printf("blockfee %f\n",blockFee*1e-6);
        }
    }

    virtual void endOutput(
        const uint8_t *p,
        uint64_t      value,
        const uint8_t *txHash,
        uint64_t      outputIndex,
        const uint8_t *outputScript,
        uint64_t      outputScriptSize
    )
    {
        if(hasGenInput && outputScriptSize == 0) {
            proofOfStake = true;
        } 
        if((proofOfStake && txCount == 2) || hasGenInput) {
            baseReward += value;
        } else {
            blockFee -= value;
            totalReceived += value;
            block_received += value;
            //if(verbose) printf("blockfee %f\n",blockFee*1e-6);
        } 
        

    }

    virtual void endBlock(
        const Block *b
    )
    {
        if(proofOfStake) {
           //update reward since we know its POS
           int64_t stakeEarned = baseReward - inputValue;
           if(stakeEarned < 0) {
               blockFee -= stakeEarned;
               baseReward = 0;
           } else  
               baseReward = stakeEarned;
           POScount++;
           totalStakeEarned += baseReward;
           //if((int)currBlock > 0)
           blkTxCount -= 2;
           txCount -= 2;
           totalTrans += txCount;
           last_pos_bits = bits;
        } else {
            last_pow_bits = bits;
            last_pow_reward = baseReward;
            POWcount++;
            totalMined += baseReward;
            blkTxCount -= 1;
            txCount -= 1;
            totalTrans += txCount;
        }
        totalFeeDestroyed += blockFee;
        if(!skip) {
             add_block();
             increment_counters();
        }
        if(((int)currBlock % 10000) == 0)
            info("processed block %d...%d/%d inserts/existing",(int)currBlock,block_inserts,block_existing);
    }

    virtual void wrapup() {
      printf("\n");
      info("last block processed: %d",(int)currBlock);
      info("inserted blocks: %d",block_inserts);
      info("existing blocks: %d",block_existing);
      info("total blocks processed: %d",block_inserts+block_existing);
      call_query("END TRANSACTION");
      sqlite3_close(db);
    }
};

static sqliteSync syncer;

