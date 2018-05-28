/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 * Copyright (c) 2016-2018 metaverse core developers (see MVS-AUTHORS)
 *
 * This file is part of metaverse.
 *
 * metaverse is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <metaverse/database/data_base.hpp>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <metaverse/bitcoin.hpp>
#include <metaverse/bitcoin/utility/path.hpp>
#include <metaverse/bitcoin/config/base16.hpp>  // used by db_metadata and push_attachment
#include <metaverse/database/memory/memory_map.hpp>
#include <metaverse/database/settings.hpp>
#include <metaverse/database/version.hpp>

namespace libbitcoin {
namespace database {

using namespace boost::filesystem;
using namespace bc::chain;
using namespace bc::wallet;
using namespace libbitcoin::config;

// BIP30 exception blocks.
// github.com/bitcoin/bips/blob/master/bip-0030.mediawiki#specification
static const config::checkpoint exception1 =
{ "00000000000a4d0a398161ffc163c503763b1f4360639393e0e4c8e300e0caec", 91842 };
static const config::checkpoint exception2 =
{ "00000000000743f190a18c5577a3c2d2a1f610ae9601ac046a38084ccb7cd721", 91880 };

bool data_base::touch_file(const path& file_path)
{
    bc::ofstream file(file_path.string());
    if (file.bad())
        return false;

    // Write one byte so file is nonzero size.
    file.write("X", 1);
    return true;
}

bool data_base::initialize(const path& prefix, const chain::block& genesis)
{
    // Create paths.
    const store paths(prefix);

    if (!paths.touch_all())
        return false;

    data_base instance(paths, 0, 0);

    if (!instance.create()) {
        return false;
    }
    auto metadata_path = prefix / db_metadata::file_name;
    auto metadata = db_metadata(db_metadata::current_version);
    data_base::write_metadata(metadata_path, metadata);
    instance.push(genesis);
    return instance.stop();
}

bool data_base::initialize_dids(const path& prefix)
{
    const store paths(prefix);
    if (paths.dids_exist())
        return true;
    if (!paths.touch_dids())
        return false;

    data_base instance(prefix, 0, 0);
    if (!instance.create_dids())
        return false;

    instance.set_blackhole_did();

    log::info(LOG_DATABASE)
        << "Upgrading did table is complete.";

    return instance.stop();
}

bool data_base::initialize_certs(const path& prefix)
{
    const store paths(prefix);
    if (paths.certs_exist())
        return true;
    if (!paths.touch_certs())
        return false;

    data_base instance(prefix, 0, 0);
    if (!instance.create_certs())
        return false;

    log::info(LOG_DATABASE)
        << "Upgrading cert table is complete.";

    return instance.stop();
}

bool data_base::upgrade_version_63(const path& prefix)
{
    auto metadata_path = prefix / db_metadata::file_name;
    if (!boost::filesystem::exists(metadata_path))
        return false;

    data_base::db_metadata metadata;
    data_base::read_metadata(metadata_path, metadata);
    if (metadata.version_.empty()) {
        return false; // no version before, initialize all intead of upgrade.
    }

    if (!initialize_dids(prefix)) {
        log::error(LOG_DATABASE)
            << "Failed to upgrade did database.";
        return false;
    }

    if (!initialize_certs(prefix)) {
        log::error(LOG_DATABASE)
            << "Failed to upgrade cert database.";
        return false;
    }

    if (metadata.version_ != db_metadata::current_version) {
        // write new db version to metadata
        metadata = db_metadata(db_metadata::current_version);
        data_base::write_metadata(metadata_path, metadata);
    }

    return true;
}

void data_base::set_admin(const std::string& name, const std::string& passwd)
{
    accounts.set_admin(name, passwd);
}

void data_base::set_blackhole_did()
{
    const std::string did_symbol = did_detail::get_blackhole_did_symbol();
    const std::string& did_address = wallet::payment_address::blackhole_address;
    did_detail diddetail(did_symbol, did_address);

    data_chunk data(did_address.begin(), did_address.end());
    short_hash hash = ripemd160_hash(data);

    output_point outpoint = { null_hash, max_uint32 };
    uint32_t output_height = max_uint32;
    uint64_t value = 0;

    push_did_detail(diddetail, hash, outpoint, output_height, value);
    synchronize_dids();
}

data_base::store::store(const path& prefix)
{
    // Hash-based lookup (hash tables).
    blocks_lookup = prefix / "block_table";
    history_lookup = prefix / "history_table";
    spends_lookup = prefix / "spend_table";
    transactions_lookup = prefix / "transaction_table";
    /* begin database for account, asset, address_asset relationship */
    accounts_lookup = prefix / "account_table";
    assets_lookup = prefix / "asset_table";  // for blockchain assets
    certs_lookup = prefix / "cert_table";   // for blockchain certs
    address_assets_lookup = prefix / "address_asset_table"; // for blockchain
    address_assets_rows = prefix / "address_asset_row"; // for blockchain
    account_assets_lookup = prefix / "account_asset_table";
    account_assets_rows = prefix / "account_asset_row";
    dids_lookup = prefix / "did_table";
    address_dids_lookup = prefix / "address_did_table"; // for blockchain
    address_dids_rows = prefix / "address_did_row"; // for blockchain
    account_addresses_lookup = prefix / "account_address_table";
    account_addresses_rows = prefix / "account_address_rows";
    /* end database for account, asset, address_asset relationship */

    // Height-based (reverse) lookup.
    blocks_index = prefix / "block_index";

    // One (address) to many (rows).
    history_rows = prefix / "history_rows";
    stealth_rows = prefix / "stealth_rows";

    // Exclusive database access reserved by this process.
    database_lock = prefix / "process_lock";
}

bool data_base::store::touch_all() const
{
    // Return the result of the database file create.
    return
        touch_file(blocks_lookup) &&
        touch_file(blocks_index) &&
        touch_file(history_lookup) &&
        touch_file(history_rows) &&
        touch_file(stealth_rows) &&
        touch_file(spends_lookup) &&
        touch_file(transactions_lookup) &&
        /* begin database for account, asset, address_asset relationship */
        touch_file(accounts_lookup) &&
        touch_file(assets_lookup) &&
        touch_file(certs_lookup) &&
        touch_file(address_assets_lookup) &&
        touch_file(address_assets_rows) &&
        touch_file(account_assets_lookup) &&
        touch_file(account_assets_rows) &&
        touch_file(dids_lookup) &&
        touch_file(address_dids_lookup) &&
        touch_file(address_dids_rows) &&
        touch_file(account_addresses_lookup) &&
        touch_file(account_addresses_rows);
        /* end database for account, asset, address_asset relationship */
}

bool data_base::store::dids_exist() const
{
    return
        boost::filesystem::exists(dids_lookup) ||
        boost::filesystem::exists(address_dids_lookup) ||
        boost::filesystem::exists(address_dids_rows);
}

bool data_base::store::touch_dids() const
{
    return
        touch_file(dids_lookup) &&
        touch_file(address_dids_lookup) &&
        touch_file(address_dids_rows);
}

bool data_base::store::certs_exist() const
{
    return boost::filesystem::exists(certs_lookup);
}

bool data_base::store::touch_certs() const
{
    return touch_file(certs_lookup);
}

data_base::db_metadata::db_metadata():version_("")
{
}

data_base::db_metadata::db_metadata(std::string version):version_(version)
{
}

void data_base::db_metadata::reset()
{
    version_ = "";
}

bool data_base::db_metadata::from_data(const data_chunk& data)
{
    data_source istream(data);
    return from_data(istream);
}

bool data_base::db_metadata::from_data(std::istream& stream)
{
    istream_reader source(stream);
    return from_data(source);
}

bool data_base::db_metadata::from_data(reader& source)
{
    reset();
    version_ = source.read_string();
    //auto result = static_cast<bool>(source);
    return true;
}

data_chunk data_base::db_metadata::to_data() const
{
    data_chunk data;
    data_sink ostream(data);
    to_data(ostream);
    ostream.flush();
    //BITCOIN_ASSERT(data.size() == serialized_size());
    return data;
}

void data_base::db_metadata::to_data(std::ostream& stream) const
{
    ostream_writer sink(stream);
    to_data(sink);
}

void data_base::db_metadata::to_data(writer& sink) const
{
    sink.write_string(version_);
}

uint64_t data_base::db_metadata::serialized_size() const
{
    return sizeof(version_);
}

#ifdef MVS_DEBUG
std::string data_base::db_metadata::to_string() const
{
    std::ostringstream ss;

    ss << "\t version = " << version_ << "\n"
        ;
    return ss.str();
}
#endif

std::istream& operator>>(std::istream& input, data_base::db_metadata& metadata)
{
    std::string hexcode;
    input >> hexcode;

    metadata.from_data(base16(hexcode));

    return input;
}

std::ostream& operator<<(std::ostream& output, const data_base::db_metadata& metadata)
{
    // tx base16 is a private encoding in bx, used to pass between commands.
    const auto bytes = metadata.to_data();
    output << base16(bytes);
    return output;
}

const std::string data_base::db_metadata::current_version = MVS_DATABASE_VERSION;
const std::string data_base::db_metadata::file_name = "metadata";

data_base::file_lock data_base::initialize_lock(const path& lock)
{
    // Touch the lock file to ensure its existence.
    const auto lock_file_path = lock.string();
    bc::ofstream file(lock_file_path, std::ios::app);
    file.close();
#ifdef UNICODE
    std::function<std::string(std::wstring)> f = [&](std::wstring wide) ->std::string {
        int ansiiLen = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        char *pAssii = new char[ansiiLen];
        WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, pAssii, ansiiLen, nullptr, nullptr);
        std::string str(pAssii);
        delete[] pAssii;
        return str;
    };
    std::string path_str = f(lock.wstring());
#else
    std::string path_str = lock_file_path;
#endif
    // BOOST:
    // Opens a file lock. Throws interprocess_exception if the file does not
    // exist or there are no operating system resources. The file lock is
    // destroyed on its destruct and does not throw.
    return file_lock(path_str.c_str());
}

void data_base::uninitialize_lock(const path& lock)
{
    // BUGBUG: Throws if the lock is not held (i.e. in error condition).
    boost::filesystem::remove(lock);
}

data_base::data_base(const settings& settings)
  : data_base(settings.directory, settings.history_start_height,
        settings.stealth_start_height)
{
}

data_base::data_base(const path& prefix, size_t history_height,
    size_t stealth_height)
  : data_base(store(prefix), history_height, stealth_height)
{
}

data_base::data_base(const store& paths, size_t history_height,
    size_t stealth_height)
  : lock_file_path_(paths.database_lock),
    history_height_(history_height),
    stealth_height_(stealth_height),
    sequential_lock_(0),
    mutex_(std::make_shared<shared_mutex>()),
    blocks(paths.blocks_lookup, paths.blocks_index, mutex_),
    history(paths.history_lookup, paths.history_rows, mutex_),
    stealth(paths.stealth_rows, mutex_),
    spends(paths.spends_lookup, mutex_),
    transactions(paths.transactions_lookup, mutex_),
    /* begin database for account, asset, address_asset, did relationship */
    accounts(paths.accounts_lookup, mutex_),
    assets(paths.assets_lookup, mutex_),
    address_assets(paths.address_assets_lookup, paths.address_assets_rows, mutex_),
    account_assets(paths.account_assets_lookup, paths.account_assets_rows, mutex_),
    certs(paths.certs_lookup, mutex_),
    dids(paths.dids_lookup, mutex_),
    address_dids(paths.address_dids_lookup, paths.address_dids_rows, mutex_),
    account_addresses(paths.account_addresses_lookup, paths.account_addresses_rows, mutex_)
    /* end database for account, asset, address_asset, did relationship */
{
}

// Close does not call stop because there is no way to detect thread join.
data_base::~data_base()
{
    close();
}

void data_base::write_metadata(const path& metadata_path, data_base::db_metadata& metadata)
{
    bc::ofstream file_output(metadata_path.string(), std::ofstream::out);
    file_output << metadata;
    file_output << std::flush;
    file_output.close();
}

void data_base::read_metadata(const path& metadata_path, data_base::db_metadata& metadata)
{
    if(!boost::filesystem::exists(metadata_path)) {
        metadata = data_base::db_metadata();
        return;
    }
    bc::ifstream file_input(metadata_path.string(), std::ofstream::in);
    if (!file_input.good()) {
        throw std::logic_error{std::string("read_metadata error : ")+ strerror(errno)};
    }
    file_input >> metadata;
    file_input.close();
}

// Startup and shutdown.
// ----------------------------------------------------------------------------

// Leaves database in started state.
// Throws if there is insufficient disk space.
// TODO: merge this with file creation (initialization above).
// This is actually first initialization of existing files, not file creation.
bool data_base::create()
{
    // Return the result of the database create.
    return
        blocks.create() &&
        history.create() &&
        spends.create() &&
        stealth.create() &&
        transactions.create() &&
        /* begin database for account, asset, address_asset relationship */
        accounts.create() &&
        assets.create() &&
        address_assets.create() &&
        account_assets.create() &&
        certs.create() &&
        dids.create() &&
        address_dids.create() &&
        account_addresses.create()
        /* end database for account, asset, address_asset relationship */
        ;
}

bool data_base::create_dids()
{
    return
        dids.create() &&
        address_dids.create();
}

bool data_base::create_certs()
{
    return
        certs.create();
}

// Start must be called before performing queries.
// Start may be called after stop and/or after close in order to restart.
bool data_base::start()
{
    // TODO: create a class to encapsulate the full file lock concept.
    file_lock_ = std::make_shared<file_lock>(initialize_lock(lock_file_path_));

    // BOOST:
    // Effects: The calling thread tries to acquire exclusive ownership of the
    // mutex without waiting. If no other thread has exclusive, or sharable
    // ownership of the mutex this succeeds. Returns: If it can acquire
    // exclusive ownership immediately returns true. If it has to wait, returns
    // false. Throws: interprocess_exception on error. Note that a file lock
    // can't guarantee synchronization between threads of the same process so
    // just use file locks to synchronize threads from different processes.
    if (!file_lock_->try_lock())
        return false;

    const auto start_exclusive = begin_write();
    const auto start_result =
        blocks.start() &&
        history.start() &&
        spends.start() &&
        stealth.start() &&
        transactions.start() &&
        /* begin database for account, asset, address_asset relationship */
        accounts.start() &&
        assets.start() &&
        address_assets.start() &&
        account_assets.start() &&
        certs.start() &&
        dids.start() &&
        address_dids.start() &&
        account_addresses.start()
        /* end database for account, asset, address_asset relationship */
        ;
    const auto end_exclusive = end_write();

    // Return the result of the database start.
    return start_exclusive && start_result && end_exclusive;
}

// Stop only accelerates work termination, only required if restarting.
bool data_base::stop()
{
    const auto start_exclusive = begin_write();
    const auto blocks_stop = blocks.stop();
    const auto history_stop = history.stop();
    const auto spends_stop = spends.stop();
    const auto stealth_stop = stealth.stop();
    const auto transactions_stop = transactions.stop();
    /* begin database for account, asset, address_asset relationship */
    const auto accounts_stop = accounts.stop();
    const auto assets_stop = assets.stop();
    const auto address_assets_stop = address_assets.stop();
    const auto account_assets_stop = account_assets.stop();
    const auto certs_stop = certs.stop();
    const auto dids_stop = dids.stop();
    const auto address_dids_stop = address_dids.stop();
    const auto account_addresses_stop = account_addresses.stop();
    /* end database for account, asset, address_asset relationship */
    const auto end_exclusive = end_write();

    // This should remove the lock file. This is not important for locking
    // purposes, but it provides a sentinel to indicate hard shutdown.
    file_lock_ = nullptr;
    uninitialize_lock(lock_file_path_);

    // Return the cumulative result of the database shutdowns.
    return
        start_exclusive &&
        blocks_stop &&
        history_stop &&
        spends_stop &&
        stealth_stop &&
        transactions_stop &&
        /* begin database for account, asset, address_asset relationship */
        accounts_stop &&
        assets_stop &&
        address_assets_stop &&
        account_assets_stop &&
        certs_stop &&
        dids_stop &&
        address_dids_stop &&
        account_addresses_stop &&
        /* end database for account, asset, address_asset relationship */
        end_exclusive;
}

// Close is optional as the database will close on destruct.
bool data_base::close()
{
    const auto blocks_close = blocks.close();
    const auto history_close = history.close();
    const auto spends_close = spends.close();
    const auto stealth_close = stealth.close();
    const auto transactions_close = transactions.close();
    /* begin database for account, asset, address_asset relationship */
    const auto accounts_close = accounts.close();
    const auto assets_close = assets.close();
    const auto address_assets_close = address_assets.close();
    const auto address_dids_close = address_dids.close();
    const auto account_assets_close = account_assets.close();
    const auto certs_close = certs.close();
    const auto dids_close = dids.close();

    const auto account_addresses_close = account_addresses.close();
    /* end database for account, asset, address_asset relationship */

    // Return the cumulative result of the database closes.
    return
        blocks_close &&
        history_close &&
        spends_close &&
        stealth_close &&
        transactions_close&&
        /* begin database for account, asset, address_asset relationship */
        accounts_close &&
        assets_close &&
        address_assets_close&&
        account_assets_close&&
        certs_close &&
        dids_close &&

        account_addresses_close
        /* end database for account, asset, address_asset relationship */
        ;
}

// Locking.
// ----------------------------------------------------------------------------

handle data_base::begin_read()
{
    return sequential_lock_.load();
}

bool data_base::is_read_valid(handle value)
{
    return value == sequential_lock_.load();
}

bool data_base::is_write_locked(handle value)
{
    return (value % 2) == 1;
}

// TODO: drop a file as a write sentinel that we can use to detect uncontrolled
// shutdown during write. Use a similar approach around initial block download.
// Fail startup if the sentinel is detected. (file: write_lock).
bool data_base::begin_write()
{
    // slock is now odd.
    return is_write_locked(++sequential_lock_);
}

// TODO: clear the write sentinel.
bool data_base::end_write()
{
    // slock_ is now even again.
    return !is_write_locked(++sequential_lock_);
}

// Query engines.
// ----------------------------------------------------------------------------

static size_t get_next_height(const block_database& blocks)
{
    size_t current_height;
    const auto empty_chain = !blocks.top(current_height);
    return empty_chain ? 0 : current_height + 1;
}

static bool is_allowed_duplicate(const header& head, size_t height)
{
    return
        (height == exception1.height() && head.hash() == exception1.hash()) ||
        (height == exception2.height() && head.hash() == exception2.hash());
}

void data_base::synchronize()
{
    spends.sync();
    history.sync();
    stealth.sync();
    transactions.sync();
    /* begin database for account, asset, address_asset relationship */
    accounts.sync();
    assets.sync();
    address_assets.sync();
    account_assets.sync();
    certs.sync();
    dids.sync();
    address_dids.sync();
    account_addresses.sync();
    /* end database for account, asset, address_asset relationship */
    blocks.sync();
}

void data_base::synchronize_dids()
{
    dids.sync();
    address_dids.sync();
}

void data_base::synchronize_certs()
{
    certs.sync();
}

void data_base::push(const block& block)
{
    // Height is unsafe unless database locked.
    push(block, get_next_height(blocks));
}

void data_base::push(const block& block, uint64_t height)
{
    for (size_t index = 0; index < block.transactions.size(); ++index)
    {
        // Skip BIP30 allowed duplicates (coinbase txs of excepted blocks).
        // We handle here because this is the lowest public level exposed.
        if (index == 0 && is_allowed_duplicate(block.header, height))
            continue;

        const auto& tx = block.transactions[index];
        const auto tx_hash = tx.hash();

        timestamp_ = block.header.timestamp; // for address_asset_database store_input/store_output used only
        // Add inputs
        if (!tx.is_coinbase())
            push_inputs(tx_hash, height, tx.inputs);

        std::string didaddress = tx.get_did_transfer_old_address();
        if (!didaddress.empty()) {
            data_chunk data(didaddress.begin(), didaddress.end());
            short_hash key = ripemd160_hash(data);
            address_dids.delete_old_did(key);
        }

        // Add outputs
        push_outputs(tx_hash, height, tx.outputs);

        // Add stealth outputs
        push_stealth(tx_hash, height, tx.outputs);

        // Add transaction
        transactions.store(height, index, tx);
    }

    // Add block itself.
    blocks.store(block, height);

    // Synchronise everything that was added.
    synchronize();
}

void data_base::push_inputs(const hash_digest& tx_hash, size_t height,
    const input::list& inputs)
{
    for (uint32_t index = 0; index < inputs.size(); ++index)
    {
        // We also push spends in the inputs loop.
        const auto& input = inputs[index];
        const chain::input_point point{ tx_hash, index };
        spends.store(input.previous_output, point);

        if (height < history_height_)
            continue;

        // Try to extract an address.
        const auto address = payment_address::extract(input.script);
        if (!address)
            continue;

        const auto& previous = input.previous_output;
        history.add_input(address.hash(), point, height, previous);

        /* begin added for asset issue/transfer */
        auto address_str = address.encoded();
        data_chunk data(address_str.begin(), address_str.end());
        short_hash key = ripemd160_hash(data);
        address_assets.store_input(key, point, height, previous, timestamp_);
        address_assets.sync();
        /* end added for asset issue/transfer */
    }
}

void data_base::push_outputs(const hash_digest& tx_hash, size_t height,
    const output::list& outputs)
{
    if (height < history_height_)
        return;

    for (uint32_t index = 0; index < outputs.size(); ++index)
    {
        const auto& output = outputs[index];
        const chain::output_point point{ tx_hash, index };

        // Try to extract an address.
        const auto address = payment_address::extract(output.script);
        if (!address)
            continue;

        const auto value = output.value;
        history.add_output(address.hash(), point, height, value);

        push_attachemnt(output.attach_data, address, point, height, value);
    }
}

void data_base::push_stealth(const hash_digest& tx_hash, size_t height,
    const output::list& outputs)
{
    if (height < stealth_height_ || outputs.empty())
        return;

    // Stealth outputs are paired by convention.
    for (size_t index = 0; index < (outputs.size() - 1); ++index)
    {
        const auto& ephemeral_script = outputs[index].script;
        const auto& payment_script = outputs[index + 1].script;

        // Try to extract an unsigned ephemeral key from the first output.
        hash_digest unsigned_ephemeral_key;
        if (!extract_ephemeral_key(unsigned_ephemeral_key, ephemeral_script))
            continue;

        // Try to extract a stealth prefix from the first output.
        uint32_t prefix;
        if (!to_stealth_prefix(prefix, ephemeral_script))
            continue;

        // Try to extract the payment address from the second output.
        const auto address = payment_address::extract(payment_script);
        if (!address)
            continue;

        // The payment address versions are arbitrary and unused here.
        const chain::stealth_compact row
        {
            unsigned_ephemeral_key,
            address.hash(),
            tx_hash
        };

        stealth.store(prefix, height, row);
    }
}

chain::block data_base::pop()
{
    size_t height;
    DEBUG_ONLY(const auto result =) blocks.top(height);
    BITCOIN_ASSERT_MSG(result, "Pop on empty database.");

    const auto block_result = blocks.get(height);
    const auto count = block_result.transaction_count();

    // Build the block for return.
    chain::block block;
    block.header = block_result.header();
    block.transactions.reserve(count);
    auto& txs = block.transactions;

    for (size_t tx = 0; tx < count; ++tx)
    {
        const auto tx_hash = block_result.transaction_hash(tx);
        const auto tx_result = transactions.get(tx_hash);

        //fix a bug ,synchronize block may destroy the database
        /*if (!tx_result) {
            continue;
        }*/

        BITCOIN_ASSERT(tx_result);
        BITCOIN_ASSERT(tx_result.height() == height);
        BITCOIN_ASSERT(tx_result.index() == static_cast<size_t>(tx));

        // TODO: the deserialization should cache the hash on the tx.
        // Deserialize the transaction and move it to the block.
        txs.emplace_back(tx_result.transaction());
    }

    // Loop txs backwards, the reverse of how they are added.
    // Remove txs, then outputs, then inputs (also reverse order).
    for (auto tx = txs.rbegin(); tx != txs.rend(); ++tx)
    {
        transactions.remove(tx->hash());
        pop_outputs(tx->outputs, height);

        if (!tx->is_coinbase())
            pop_inputs(tx->inputs, height);
    }

    // Stealth unlink is not implemented.
    stealth.unlink(height);
    blocks.unlink(height);
    blocks.remove(block.header.hash()); // wdy remove block from block hash table

    // Synchronise everything that was changed.
    synchronize();

    // Return the block.
    return block;
}

void data_base::pop_inputs(const input::list& inputs, size_t height)
{
    // Loop in reverse.
    for (auto input = inputs.rbegin(); input != inputs.rend(); ++input)
    {
        spends.remove(input->previous_output);

        if (height < history_height_)
            continue;

        // Try to extract an address.
        const auto address = payment_address::extract(input->script);

        if (address) {
            history.delete_last_row(address.hash());
            // delete address asset record
            auto address_str = address.encoded();
            data_chunk data(address_str.begin(), address_str.end());
            short_hash hash = ripemd160_hash(data);
            address_assets.delete_last_row(hash);
        }
    }
}

void data_base::pop_outputs(const output::list& outputs, size_t height)
{
    if (height < history_height_)
        return;

    // Loop in reverse.
    for (auto output = outputs.rbegin(); output != outputs.rend(); ++output)
    {
        // Try to extract an address.
        const auto address = payment_address::extract(output->script);

        if (address) {
            history.delete_last_row(address.hash());
            // delete address asset record
            auto address_str = address.encoded();
            data_chunk data(address_str.begin(), address_str.end());
            short_hash hash = ripemd160_hash(data);
            address_assets.delete_last_row(hash);
            // remove asset or did from database
            bc::chain::output op = *output;
            if (op.is_asset_issue() || op.is_asset_secondaryissue()) {
                auto symbol = op.get_asset_symbol();
                const data_chunk& symbol_data = data_chunk(symbol.begin(), symbol.end());
                const auto symbol_hash = sha256_hash(symbol_data);
                assets.remove(symbol_hash);
            }
            else if (op.is_did()) {
                auto symbol = op.get_did_symbol();
                const data_chunk& symbol_data = data_chunk(symbol.begin(), symbol.end());
                const auto symbol_hash = sha256_hash(symbol_data);

                if(op.is_did_issue())
                {
                    address_dids.delete_last_row(hash);
                    address_dids.sync();
                    dids.remove(symbol_hash);
                    dids.sync();
                }
                else if(op.is_did_transfer() )
                {
                    std::shared_ptr<blockchain_did> blockchain_did_=  dids.pop_did_transfer(symbol_hash);
                    dids.sync();     

                    if(blockchain_did_)
                    {   
                        auto old_address = blockchain_did_->get_did().get_address();
                        data_chunk data_old(old_address.begin(), old_address.end());
                        short_hash old_hash = ripemd160_hash(data_old);

                        address_dids.delete_last_row(old_hash);
                        address_dids.delete_last_row(hash);
                        
                        address_dids.store_output(old_hash, blockchain_did_->get_tx_point(), blockchain_did_->get_height(), 0,
                            static_cast<typename std::underlying_type<business_kind>::type>(business_kind::did_issue),
                            timestamp_, blockchain_did_->get_did());
                        address_dids.sync();

                    }
                }

            }
            else if (op.is_asset_cert()) {
                const auto asset_cert = op.get_asset_cert();
                if (asset_cert.is_newly_generated()) {
                    const auto key_str = asset_cert.get_key();
                    const data_chunk& data = data_chunk(key_str.begin(), key_str.end());
                    const auto key_hash = sha256_hash(data);
                    certs.remove(key_hash);
                }
            }
        }
    }
}

/* begin store asset related info into database */
#include <metaverse/bitcoin/config/base16.hpp>
using namespace libbitcoin::config;

void data_base::push_attachemnt(const attachment& attach, const payment_address& address,
    const output_point& outpoint, uint32_t output_height, uint64_t value)
{
    auto address_str = address.encoded();
    log::trace(LOG_DATABASE) << "push_attachemnt address_str=" << address_str;
    log::trace(LOG_DATABASE) << "push_attachemnt address hash=" << base16(address.hash());
    data_chunk data(address_str.begin(), address_str.end());
    short_hash hash = ripemd160_hash(data);
    auto visitor = attachment_visitor(this, hash, outpoint, output_height, value);
    boost::apply_visitor(visitor, const_cast<attachment&>(attach).get_attach());
}

void data_base::push_etp(const etp& etp, const short_hash& key,
    const output_point& outpoint, uint32_t output_height, uint64_t value)
{
    address_assets.store_output(key, outpoint, output_height, value,
        static_cast<typename std::underlying_type<business_kind>::type>(business_kind::etp),
        timestamp_, etp);
    address_assets.sync();

}

void data_base::push_etp_award(const etp_award& award, const short_hash& key,
    const output_point& outpoint, uint32_t output_height, uint64_t value)
{
    address_assets.store_output(key, outpoint, output_height, value,
        static_cast<typename std::underlying_type<business_kind>::type>(business_kind::etp_award),
        timestamp_, award);
    address_assets.sync();
}

void data_base::push_message(const chain::blockchain_message& msg, const short_hash& key,
    const output_point& outpoint, uint32_t output_height, uint64_t value)
{
    address_assets.store_output(key, outpoint, output_height, value,
        static_cast<typename std::underlying_type<business_kind>::type>(business_kind::message),
        timestamp_, msg);
    address_assets.sync();
}

void data_base::push_asset(const asset& sp, const short_hash& key,
    const output_point& outpoint, uint32_t output_height, uint64_t value) // sp = smart property
{
    auto visitor = asset_visitor(this, key, outpoint, output_height, value);
    boost::apply_visitor(visitor, const_cast<asset&>(sp).get_data());
}

void data_base::push_asset_cert(const asset_cert& sp_cert, const short_hash& key,
    const output_point& outpoint, uint32_t output_height, uint64_t value)
{
    if (sp_cert.is_newly_generated()) {
        certs.store(sp_cert);
        certs.sync();
    }
    address_assets.store_output(key, outpoint, output_height, value,
        static_cast<typename std::underlying_type<business_kind>::type>(business_kind::asset_cert),
        timestamp_, sp_cert);
    address_assets.sync();
}

void data_base::push_asset_detail(const asset_detail& sp_detail, const short_hash& key,
    const output_point& outpoint, uint32_t output_height, uint64_t value)
{
    const data_chunk& data = data_chunk(sp_detail.get_symbol().begin(), sp_detail.get_symbol().end());
    const auto hash = sha256_hash(data);
    auto bc_asset = blockchain_asset(0, outpoint,output_height, sp_detail);
    assets.store(hash, bc_asset);
    assets.sync();
    address_assets.store_output(key, outpoint, output_height, value,
        static_cast<typename std::underlying_type<business_kind>::type>(business_kind::asset_issue),
        timestamp_, sp_detail);
    address_assets.sync();
}

void data_base::push_asset_transfer(const asset_transfer& sp_transfer, const short_hash& key,
    const output_point& outpoint, uint32_t output_height, uint64_t value)
{
    address_assets.store_output(key, outpoint, output_height, value,
        static_cast<typename std::underlying_type<business_kind>::type>(business_kind::asset_transfer),
        timestamp_, sp_transfer);
    address_assets.sync();
}
/* end store asset related info into database */

/* begin store did related info into database */
void data_base::push_did(const did& sp, const short_hash& key,
    const output_point& outpoint, uint32_t output_height, uint64_t value) // sp = smart property
{
    push_did_detail(sp.get_data(), key, outpoint, output_height, value);
}

void data_base::push_did_detail(const did_detail& sp_detail, const short_hash& key,
    const output_point& outpoint, uint32_t output_height, uint64_t value)
{
    const data_chunk& data = data_chunk(sp_detail.get_symbol().begin(), sp_detail.get_symbol().end());
    const auto hash = sha256_hash(data);
    auto bc_did = blockchain_did(0, outpoint,output_height, blockchain_did::address_current,sp_detail);
    dids.store(hash, bc_did);
    dids.sync();
    address_dids.store_output(key, outpoint, output_height, value,
        static_cast<typename std::underlying_type<business_kind>::type>(business_kind::did_issue),
        timestamp_, sp_detail);
    address_dids.sync();
}

/* end store did related info into database */

} // namespace data_base
} // namespace libbitcoin

