#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
#include <order_book/itch_market.hpp>
#include <order_book/levels.hpp>
#include <order_book/order_book.hpp>
#include <order_book/replay.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <ios>
#include <istream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace order_book {

namespace {

// Workers receive the input in batches of whole records of at least this size
constexpr std::size_t batch_size = std::size_t{1} << 20U;
// Batches read ahead of the one being written out
constexpr std::size_t batches_in_flight = 4;

constexpr std::size_t max_digits = std::numeric_limits<std::uint64_t>::digits10 + 1;

char* write_uint(char* out, std::uint64_t value)
{
    return std::to_chars(out, out + max_digits, value).ptr;
}

char* write_level(char* out, const std::optional<Level>& level)
{
    if (level) {
        out = write_uint(out, level->price);
        *out++ = ',';
        out = write_uint(out, level->quantity);
    }
    else {
        *out++ = ',';
    }
    return out;
}

// Append `<TIME>,<TICKER>,<BBP>,<BBQ>,<BAP>,<BAQ>` and a newline in one write, rather than a
// std::string call per field
void append_line(std::string& text, std::uint64_t time, std::string_view ticker, const Bbo& bbo)
{
    const auto size = text.size();
    const auto longest = size + ticker.size() + (5 * max_digits) + 6;
    if (longest > text.capacity()) {
        text.reserve(std::max(longest, 2 * text.capacity()));
    }
    text.resize_and_overwrite(longest, [&](char* data, std::size_t) {
        auto* out = write_uint(data + size, time);
        *out++ = ',';
        out = std::ranges::copy(ticker, out).out;
        *out++ = ',';
        out = write_level(out, bbo.bid);
        *out++ = ',';
        out = write_level(out, bbo.ask);
        *out++ = '\n';
        return static_cast<std::size_t>(out - data);
    });
}

std::string describe(BookError error, std::string_view order)
{
    return std::format("{} order '{}'",
                       error == BookError::DuplicateOrder ? "duplicate" : "unknown", order);
}

std::invalid_argument record_error(std::string_view unit, std::size_t number,
                                   std::string_view message)
{
    const auto what = std::format("{} {}: {}", unit, number, message);
    return std::invalid_argument(what);
}

// Second field of a line, which names the ticker in any line that parses
std::string_view ticker_field(std::string_view line)
{
    const auto comma = line.find(',');
    if (comma == std::string_view::npos) {
        return {};
    }
    const auto rest = line.substr(comma + 1);
    return rest.substr(0, rest.find(','));
}

// Finds where the last whole line of a batch ends
class LineFraming {
public:
    // Offset just past the last LF in `text`, of which [0, from) was scanned before; npos if none
    std::size_t scan(std::string_view text, std::size_t from)
    {
        if (const auto found = text.substr(from).rfind('\n'); found != std::string_view::npos) {
            m_end = from + found + 1;
        }
        return m_end;
    }

private:
    std::size_t m_end = std::string_view::npos;
};

// 2-byte big-endian length that precedes each ITCH message
std::size_t message_length(const char* prefix)
{
    std::uint16_t length = 0;
    std::memcpy(&length, prefix, sizeof(length));
    if constexpr (std::endian::native == std::endian::little) {
        length = std::byteswap(length);
    }
    return length;
}

constexpr std::size_t length_size = 2;

// Finds where the last whole length-prefixed message of a batch ends, keeping where each starts
class MessageFraming {
public:
    // Offset just past the last whole message in `text`; npos if none
    std::size_t scan(std::string_view text, std::size_t /*from*/)
    {
        while (m_end + length_size <= text.size()) {
            const auto next = m_end + length_size + message_length(text.data() + m_end);
            if (next > text.size()) {
                break;
            }
            // A batch is a few MiB, so its offsets fit 32 bits
            m_starts.push_back(static_cast<std::uint32_t>(m_end));
            m_end = next;
        }
        return m_end == 0 ? std::string_view::npos : m_end;
    }
    // Offset just past the last whole message scanned, 0 if none
    [[nodiscard]] std::size_t end() const { return m_end; }
    // Offset of each whole message's length
    [[nodiscard]] std::vector<std::uint32_t>& starts() { return m_starts; }

private:
    std::size_t m_end = 0;
    std::vector<std::uint32_t> m_starts;
};

// Cuts a stream into batches of whole records, lines or messages as Framing finds them
template <typename Framing> class BatchReader {
public:
    explicit BatchReader(std::istream& in) : m_source(in.rdbuf()), m_done(!in) {}

    // Replace `text` with at least batch_size bytes of whole records, fewer at the end of the
    // stream, where the last record may be incomplete, as a fresh `framing` finds them. Empty once
    // the stream is exhausted. A read that throws ends the stream after the last whole record
    // before it, and failure() then holds the exception.
    void next(std::string& text, Framing& framing)
    {
        text = std::exchange(m_carry, {});
        auto end = std::string::npos;
        while (!m_done) {
            const auto size = text.size();
            text.resize_and_overwrite(size + batch_size, [&](char* data, std::size_t) {
                // resize_and_overwrite() leaves the string undefined if this throws
                std::streamsize count = 0;
                try {
                    count = m_source->sgetn(data + size, static_cast<std::streamsize>(batch_size));
                }
                catch (...) {
                    m_failure = std::current_exception();
                }
                // A stream buffer may return less than asked before its end, so only an empty
                // read ends the input
                m_done = count == 0;
                return size + static_cast<std::size_t>(count);
            });
            end = framing.scan(text, size);
            if (m_failure) {
                text.resize(end == std::string::npos ? 0 : end);
                return;
            }
            if (text.size() >= batch_size && end != std::string::npos) {
                m_carry.assign(text, end);
                text.resize(end);
                return;
            }
        }
    }

    // What a read of the stream threw, if one did
    [[nodiscard]] const std::exception_ptr& failure() const { return m_failure; }

private:
    std::streambuf* m_source;
    bool m_done;
    std::exception_ptr m_failure;
    // Start of a record that continues into the next batch
    std::string m_carry;
};

// What one worker made of one batch
struct Output {
    std::string text;
    // Number of the record that wrote each line of text, and the offset just past the line
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    std::size_t events = 0;
    // Number of the first record that could not be parsed or applied, and why
    std::optional<std::pair<std::size_t, std::string>> failure;
    std::exception_ptr exception;
};

// A line of a batch with its number in the feed
struct Line {
    std::size_t number;
    std::string_view text;
};

struct Batch {
    std::string text;
    // CSV: each worker's lines, in feed order, viewing text
    std::vector<std::vector<Line>> lines;
    // Number of the first record and how many there are, the failing one included
    std::size_t first = 0;
    std::size_t records = 0;
    // ITCH: where each whole message's length is in text, which every worker reads to find the
    // messages of its locates
    std::vector<std::uint32_t> starts;
    std::vector<Output> outputs;
    // A record the batch could not be split into, with its number; no record after it is assigned
    std::optional<std::pair<std::size_t, std::string>> failure;
    // Workers yet to finish the batch, guarded by the pipeline's mutex
    std::size_t pending;
};

// Split the batch into lines numbered from `number`, skipping the header and blank lines, and give
// each line to the worker its ticker hashes to. Returns the number of the next batch's first line.
std::size_t assign_lines(Batch& batch, std::size_t number, LineFraming&& /*framing*/)
{
    const auto workers = batch.outputs.size();
    batch.lines.resize(workers);
    std::string_view rest = batch.text;
    while (!rest.empty()) {
        const auto newline = rest.find('\n');
        auto line = rest.substr(0, newline);
        rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
        const auto current = number++;
        // Line 1 is the header
        if (current == 1) {
            continue;
        }
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.find_last_not_of(" \t\n\v\f\r") == std::string_view::npos) {
            continue;
        }
        const auto worker =
            workers == 1 ? 0 : std::hash<std::string_view>{}(ticker_field(line)) % workers;
        batch.lines[worker].push_back(Line{.number = current, .text = line});
    }
    return number;
}

// Number the batch's messages from `number`, as `framing` found them, and record a truncated last
// message as the batch's failure. Returns the number of the next batch's first message.
std::size_t assign_messages(Batch& batch, std::size_t number, MessageFraming&& framing)
{
    batch.starts = std::move(framing.starts());
    number += batch.starts.size();
    const auto rest = std::string_view(batch.text).substr(framing.end());
    if (rest.size() >= length_size) {
        const auto length = message_length(batch.text.data() + framing.end());
        batch.failure.emplace(number, std::format("truncated message: {} of {} bytes",
                                                  rest.size() - length_size, length));
        return number + 1;
    }
    if (!rest.empty()) {
        batch.failure.emplace(number, "truncated message length");
        return number + 1;
    }
    return number;
}

// Keeps the books of the tickers whose lines it is given
class CsvWorker {
public:
    // Apply worker `index`'s lines of the batch in order. False after a failure, which leaves the
    // books unusable.
    bool process(const Batch& batch, std::size_t index, Output& output)
    {
        return std::ranges::all_of(batch.lines[index], [&](const Line& line) {
            return apply(line.text, line.number, output);
        });
    }

private:
    bool apply(std::string_view line, std::size_t number, Output& output)
    {
        const auto message = parse_line(line);
        if (!message) {
            output.failure.emplace(number, message.error());
            return false;
        }
        auto book = m_books.find(message->ticker);
        if (book == m_books.end()) {
            book = m_books.emplace(std::string(message->ticker), OrderBook{}).first;
        }

        const auto applied = book->second.apply(message->order, message->event);
        if (!applied) {
            output.failure.emplace(number, describe(applied.error(), message->order));
            return false;
        }
        if (*applied) {
            append_line(output.text, message->time, message->ticker, book->second.bbo());
            output.lines.emplace_back(number, output.text.size());
        }
        ++output.events;
        return true;
    }

    ankerl::unordered_dense::map<std::string, OrderBook, StringHash, std::equal_to<>> m_books;
};

// The worker of stock locate `locate`. A locate's number says nothing about its volume, so the
// split balances only on average; hashing the number keeps the workers' shares from following any
// pattern in how the exchange numbers its locates, as a remainder would.
// A locate and a worker count, which the check sees as swappable
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::size_t owner(std::size_t locate, std::size_t workers)
{
    // Knuth's multiplicative hashing (TAOCP vol. 3, 6.4): the multiplier is the prime nearest 2^32
    // divided by the golden ratio, so the 32-bit product spreads consecutive locates apart
    constexpr std::uint32_t multiplier = 0x9E3779B1U;
    // The hash read as a fraction of 2^32, times the worker count: the product's high 32 bits are
    // the worker, each taking an equal slice of the hash's range, without a division
    constexpr unsigned hash_bits = 32;
    const auto hash = static_cast<std::uint32_t>(locate) * multiplier;
    return static_cast<std::size_t>((std::uint64_t{hash} * workers) >> hash_bits);
}

// Keeps the books of the stock locates whose messages it is given
class ItchWorker {
public:
    // Apply the batch's messages whose stock locate selects worker `index`, in order. Messages too
    // short to name a locate go to worker 0, which reports them. False after a failure, which
    // leaves the books unusable.
    bool process(const Batch& batch, std::size_t index, Output& output)
    {
        const auto workers = batch.outputs.size();
        const auto* const text = batch.text.data();
        // Pick out this worker's messages without a branch per message, as most are another's
        m_mine.resize(batch.starts.size());
        std::size_t count = 0;
        for (std::size_t record = 0; record < batch.starts.size(); ++record) {
            const auto* const message = text + batch.starts[record];
            const auto worker = workers > 1 && message_length(message) > length_size
                                    ? owner(message_length(message + length_size + 1), workers)
                                    : 0;
            m_mine[count] = static_cast<std::uint32_t>(record);
            count += worker == index ? 1 : 0;
        }
        const auto bytes = [&](std::size_t mine) {
            const auto* const message = text + batch.starts[m_mine[mine]];
            return std::string_view(message + length_size, message_length(message));
        };
        for (std::size_t mine = 0; mine < count; ++mine) {
            // Applying a message is a chain of dependent loads: the order's index entry holds its
            // level's handle, and the level names the book side to update. On the real day many
            // of them miss the cache. A message is several hundred instructions, about as many
            // as the core keeps in flight, so it cannot start the next message's loads while
            // this one waits, and the misses are served one after another. Starting the loads of
            // later messages here overlaps them instead. The two stages follow the chain: the
            // index entry and the book, whose addresses the message gives, then the levels,
            // whose handles those loads have brought in by the time the second stage runs.
            constexpr auto distance = ItchMarket::prefetch_distance;
            if (mine + distance < count) {
                const auto ahead = peek_itch(bytes(mine + distance));
                m_market.prefetch(ahead.order, ahead.locate);
            }
// Level prefetch slowed the measured synthetic Linux x86-64 replay workload
#if !defined(__linux__) || !defined(__x86_64__)
            if (mine + (distance / 2) < count) {
                const auto ahead = peek_itch(bytes(mine + (distance / 2)));
                m_market.prefetch_levels(ahead.order, ahead.locate);
            }
#endif
            if (!apply(bytes(mine), batch.first + m_mine[mine], output)) {
                return false;
            }
        }
        return true;
    }

private:
    // Applies one decoded message, recording its output or its failure; false after a failure
    class Handler {
    public:
        Handler(ItchWorker& worker, std::size_t number, Output& output)
            : m_worker(worker), m_number(number), m_output(output)
        {
        }

        bool add(const ItchMessage& message)
        {
            auto& tickers = m_worker.m_tickers;
            if (message.locate >= tickers.size()) {
                tickers.resize(message.locate + std::size_t{1});
            }
            if (auto& ticker = tickers[message.locate]; ticker.empty()) {
                ticker = message.stock.substr(0, message.stock.find_last_not_of(' ') + 1);
            }
            return applied(message, m_worker.m_market.add(message), message.order);
        }
        bool execute(const ItchMessage& message)
        {
            return applied(message, m_worker.m_market.execute(message), message.order);
        }
        bool remove(const ItchMessage& message)
        {
            return applied(message, m_worker.m_market.remove(message), message.order);
        }
        bool replace(const ItchMessage& message)
        {
            const auto result = m_worker.m_market.replace(message);
            // A replacement fails as a duplicate under its new reference number
            const auto duplicate = !result && result.error() == BookError::DuplicateOrder;
            return applied(message, result, duplicate ? message.replacement : message.order);
        }
        bool trade(const ItchMessage& /*message*/)
        {
            ++m_output.events;
            return true;
        }
        static bool skipped() { return true; }
        bool error(const ItchError& error)
        {
            auto what = describe(error);
            m_output.failure.emplace(m_number, std::move(what));
            return false;
        }

    private:
        bool applied(const ItchMessage& message, const std::expected<bool, BookError>& result,
                     std::uint64_t order)
        {
            if (!result) {
                m_output.failure.emplace(m_number, describe(result.error(), std::to_string(order)));
                return false;
            }
            if (*result) {
                append_line(m_output.text, message.time, m_worker.m_tickers[message.locate],
                            m_worker.m_market.bbo(message.locate));
                m_output.lines.emplace_back(m_number, m_output.text.size());
            }
            ++m_output.events;
            return true;
        }

        ItchWorker& m_worker;
        std::size_t m_number;
        Output& m_output;
    };

    bool apply(std::string_view bytes, std::size_t number, Output& output)
    {
        return visit_itch(bytes, Handler(*this, number, output));
    }

    ItchMarket m_market;
    // Stock symbol of each locate's first add
    std::vector<std::string> m_tickers;
    // Records of the current batch that are this worker's
    std::vector<std::uint32_t> m_mine;
};

// Runs the workers over a queue of batches, which every worker processes in order
template <typename Worker> class Pipeline {
public:
    explicit Pipeline(std::size_t workers)
    {
        try {
            for (std::size_t index = 0; index < workers; ++index) {
                m_threads.emplace_back([this, index] { work(index); });
            }
        }
        // Only a failure to start a thread gets here; the started ones must not wait forever
        // GCOVR_EXCL_START
        catch (...) {
            stop();
            throw;
        }
        // GCOVR_EXCL_STOP
    }

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    // The threads join as m_threads is destroyed, after this releases them
    ~Pipeline() { stop(); }

    void push(std::shared_ptr<Batch> batch)
    {
        {
            const std::scoped_lock lock(m_mutex);
            m_queue.push_back(std::move(batch));
        }
        m_changed.notify_all();
    }

    // The oldest batch, once every worker has finished it
    std::shared_ptr<Batch> pop()
    {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [this] { return m_queue.front()->pending == 0; });
        auto batch = std::move(m_queue.front());
        m_queue.pop_front();
        ++m_popped;
        return batch;
    }

private:
    void stop()
    {
        {
            const std::scoped_lock lock(m_mutex);
            m_stopping = true;
        }
        m_changed.notify_all();
    }

    void work(std::size_t index)
    {
        Worker worker;
        for (std::size_t sequence = 0;; ++sequence) {
            std::shared_ptr<Batch> batch;
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock,
                               [&] { return m_stopping || sequence < m_popped + m_queue.size(); });
                if (m_stopping) {
                    return;
                }
                batch = m_queue[sequence - m_popped];
            }
            auto& output = batch->outputs[index];
            bool processed = false;
            try {
                processed = worker.process(*batch, index, output);
            }
            // Parse and apply errors are failures, so only resource exhaustion gets here
            // GCOVR_EXCL_START
            catch (...) {
                output.exception = std::current_exception();
            }
            // GCOVR_EXCL_STOP
            bool completed;
            {
                const std::scoped_lock lock(m_mutex);
                completed = --batch->pending == 0;
            }
            if (completed) {
                m_changed.notify_all();
            }
            if (!processed) {
                return;
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_changed;
    // Batches not yet popped, oldest first
    std::deque<std::shared_ptr<Batch>> m_queue;
    std::size_t m_popped = 0;
    bool m_stopping = false;
    std::vector<std::jthread> m_threads;
};

// Write the batch's output in record order and return its event count. Throws for the batch's
// first failing record, naming it by `unit`, after writing the output before it, and throws
// std::runtime_error if `out` fails.
std::size_t write_batch(Batch& batch, std::ostream& out, std::string& buffer, std::string_view unit)
{
    const std::pair<std::size_t, std::string>* failure = batch.failure ? &*batch.failure : nullptr;
    std::size_t events = 0;
    for (const auto& output : batch.outputs) {
        if (output.exception) {
            std::rethrow_exception(output.exception); // GCOVR_EXCL_LINE
        }
        if (output.failure && (failure == nullptr || output.failure->first < failure->first)) {
            failure = &*output.failure;
        }
        events += output.events;
    }
    const auto stop = failure != nullptr ? failure->first - batch.first : batch.records;

    // Which worker, plus one, wrote a line for each record, so that the lines are copied in record
    // order without comparing the workers' next lines
    std::vector<std::uint32_t> owners(batch.records, 0);
    for (std::size_t worker = 0; worker < batch.outputs.size(); ++worker) {
        for (const auto& line : batch.outputs[worker].lines) {
            owners[line.first - batch.first] = static_cast<std::uint32_t>(worker + 1);
        }
    }
    buffer.clear();
    std::vector<std::size_t> next(batch.outputs.size(), 0);
    for (std::size_t record = 0; record < stop; ++record) {
        if (owners[record] == 0) {
            continue;
        }
        const auto& output = batch.outputs[owners[record] - 1];
        auto& position = next[owners[record] - 1];
        const auto begin = position == 0 ? 0 : output.lines[position - 1].second;
        buffer.append(output.text, begin, output.lines[position].second - begin);
        ++position;
    }
    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (!out) {
        throw std::runtime_error("cannot write the output");
    }

    if (failure != nullptr) {
        throw record_error(unit, failure->first, failure->second);
    }
    return events;
}

// Replay with Framing to cut batches, assign to split them, and Worker to apply records
template <typename Framing, typename Worker>
std::size_t run(std::istream& in, std::ostream& out, std::size_t threads,
                std::size_t (*assign)(Batch&, std::size_t, Framing&&), std::string_view unit)
{
    const auto workers = std::max<std::size_t>(threads, 1);
    BatchReader<Framing> reader(in);
    Pipeline<Worker> pipeline(workers);
    std::string buffer;
    std::size_t next_record = 1;
    std::size_t queued = 0;
    bool reading = true;
    std::size_t events = 0;
    while (true) {
        while (reading && queued < batches_in_flight) {
            auto batch = std::make_shared<Batch>();
            batch->outputs.resize(workers);
            batch->pending = workers;
            batch->first = next_record;
            Framing framing;
            reader.next(batch->text, framing);
            // An empty batch assigns nothing
            next_record = assign(*batch, next_record, std::move(framing));
            if (batch->text.empty()) {
                reading = false;
                break;
            }
            batch->records = next_record - batch->first;
            pipeline.push(std::move(batch));
            ++queued;
        }
        if (queued == 0) {
            if (reader.failure()) {
                std::rethrow_exception(reader.failure());
            }
            if (!out.flush()) {
                throw std::runtime_error("cannot write the output");
            }
            return events;
        }
        events += write_batch(*pipeline.pop(), out, buffer, unit);
        --queued;
    }
}

} // namespace

std::size_t replay(std::istream& in, std::ostream& out)
{
    return replay(in, out, Format::Csv);
}

std::size_t replay(std::istream& in, std::ostream& out, std::size_t threads)
{
    return replay(in, out, Format::Csv, threads);
}

std::size_t replay(std::istream& in, std::ostream& out, Format format)
{
    constexpr auto max_workers = 8U;
    const auto workers = std::clamp(std::thread::hardware_concurrency(), 1U, max_workers);
    return replay(in, out, format, workers);
}

std::size_t replay(std::istream& in, std::ostream& out, Format format, std::size_t threads)
{
    if (format == Format::Itch) {
        return run<MessageFraming, ItchWorker>(in, out, threads, assign_messages, "message");
    }
    return run<LineFraming, CsvWorker>(in, out, threads, assign_lines, "line");
}

} // namespace order_book
