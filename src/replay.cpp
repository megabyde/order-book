#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
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
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace order_book {

namespace {

// Workers receive the input in batches of whole lines of at least this size
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

// Finds where the last whole length-prefixed message of a batch ends
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
            m_end = next;
        }
        return m_end == 0 ? std::string_view::npos : m_end;
    }

private:
    std::size_t m_end = 0;
};

// Cuts a stream into batches of whole records, lines or messages as Framing finds them
template <typename Framing> class BatchReader {
public:
    explicit BatchReader(std::istream& in) : m_source(in.rdbuf()), m_done(!in) {}

    // Replace `text` with at least batch_size bytes of whole records, fewer at the end of the
    // stream, where the last record may be incomplete. Empty once the stream is exhausted.
    void next(std::string& text)
    {
        text = std::exchange(m_carry, {});
        Framing framing;
        auto end = std::string::npos;
        while (!m_done) {
            const auto size = text.size();
            text.resize_and_overwrite(size + batch_size, [&](char* data, std::size_t) {
                // A stream buffer may return less than asked before its end, so only an empty
                // read ends the input
                const auto count =
                    m_source->sgetn(data + size, static_cast<std::streamsize>(batch_size));
                m_done = count == 0;
                return size + static_cast<std::size_t>(count);
            });
            end = framing.scan(text, size);
            if (text.size() >= batch_size && end != std::string::npos) {
                m_carry.assign(text, end);
                text.resize(end);
                return;
            }
        }
    }

private:
    std::streambuf* m_source;
    bool m_done;
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

// A record of a batch, a line or a message without its length, with its number in the feed
struct Line {
    std::size_t number;
    std::string_view text;
};

struct Batch {
    std::string text;
    // Each worker's records, in feed order, viewing text
    std::vector<std::vector<Line>> lines;
    std::vector<Output> outputs;
    // A record the batch could not be split into, with its number; no record after it is assigned
    std::optional<std::pair<std::size_t, std::string>> failure;
    // Workers yet to finish the batch, guarded by the pipeline's mutex
    std::size_t pending;
};

// Split the batch into lines numbered from `number`, skipping the header and blank lines, and give
// each line to the worker its ticker hashes to. Returns the number of the next batch's first line.
std::size_t assign_lines(Batch& batch, std::size_t number)
{
    const auto workers = batch.lines.size();
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

// Split the batch into messages numbered from `number`, and give each to the worker its stock
// locate selects. Returns the number of the next batch's first message.
std::size_t assign_messages(Batch& batch, std::size_t number)
{
    const auto workers = batch.lines.size();
    const std::string_view text = batch.text;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto current = number++;
        if (offset + length_size > text.size()) {
            batch.failure.emplace(current, "truncated message length");
            return number;
        }
        const auto length = message_length(text.data() + offset);
        const auto body = text.substr(offset + length_size, length);
        if (body.size() < length) {
            batch.failure.emplace(
                current, std::format("truncated message: {} of {} bytes", body.size(), length));
            return number;
        }
        offset += length_size + length;
        // Messages too short to name a locate go to worker 0, which reports them
        std::size_t worker = 0;
        if (workers > 1 && body.size() > length_size) {
            worker = message_length(body.data() + 1) % workers;
        }
        batch.lines[worker].push_back(Line{.number = current, .text = body});
    }
    return number;
}

// Keeps the books of the tickers whose lines it is given
class CsvWorker {
public:
    // Apply the lines in order. False after a failure, which leaves the books unusable.
    bool process(const std::vector<Line>& lines, Output& output)
    {
        return std::ranges::all_of(
            lines, [&](const Line& line) { return apply(line.text, line.number, output); });
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

    std::unordered_map<std::string, OrderBook, StringHash, std::equal_to<>> m_books;
};

// Keeps the books of the stock locates whose messages it is given
class ItchWorker {
public:
    // Apply the messages in order. False after a failure, which leaves the books unusable.
    bool process(const std::vector<Line>& messages, Output& output)
    {
        return std::ranges::all_of(messages, [&](const Line& message) {
            return apply(message.text, message.number, output);
        });
    }

private:
    struct Stock {
        std::string ticker;
        ItchOrderBook book;
    };

    bool apply(std::string_view bytes, std::size_t number, Output& output)
    {
        const auto message = decode_itch(bytes);
        if (!message) {
            output.failure.emplace(number, message.error());
            return false;
        }
        if (std::holds_alternative<Skipped>(message->event)) {
            return true;
        }
        if (message->locate >= m_stocks.size()) {
            m_stocks.resize(message->locate + std::size_t{1});
        }
        auto& stock = m_stocks[message->locate];

        std::expected<bool, BookError> applied = false;
        if (const auto* const add = std::get_if<AddOrder>(&message->event)) {
            if (stock.ticker.empty()) {
                stock.ticker = message->stock;
            }
            applied = stock.book.apply(message->order, *add);
        }
        else if (const auto* const replace = std::get_if<ReplaceOrder>(&message->event)) {
            applied =
                stock.book.replace(message->order, replace->order, replace->price, replace->shares);
            if (!applied && applied.error() == BookError::DuplicateOrder) {
                output.failure.emplace(number,
                                       describe(applied.error(), std::to_string(replace->order)));
                return false;
            }
        }
        else if (const auto* const execute = std::get_if<ExecuteOrder>(&message->event)) {
            applied = stock.book.apply(message->order, *execute);
        }
        else if (std::holds_alternative<DeleteOrder>(message->event)) {
            applied = stock.book.apply(message->order, DeleteOrder{});
        }
        if (!applied) {
            output.failure.emplace(number,
                                   describe(applied.error(), std::to_string(message->order)));
            return false;
        }
        if (*applied) {
            append_line(output.text, message->time, stock.ticker, stock.book.bbo());
            output.lines.emplace_back(number, output.text.size());
        }
        ++output.events;
        return true;
    }

    // Indexed by stock locate
    std::vector<Stock> m_stocks;
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
                processed = worker.process(batch->lines[index], output);
            }
            // Parse and apply errors are failures, so only resource exhaustion gets here
            // GCOVR_EXCL_START
            catch (...) {
                output.exception = std::current_exception();
            }
            // GCOVR_EXCL_STOP
            {
                const std::scoped_lock lock(m_mutex);
                --batch->pending;
            }
            m_changed.notify_all();
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
// first failing record, naming it by `unit`, after writing the output before it.
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
    const auto limit =
        failure != nullptr ? failure->first : std::numeric_limits<std::size_t>::max();

    // Merge the workers' lines, each list already in line order
    buffer.clear();
    std::vector<std::size_t> next(batch.outputs.size(), 0);
    while (true) {
        const Output* earliest = nullptr;
        std::size_t* position = nullptr;
        for (std::size_t worker = 0; worker < next.size(); ++worker) {
            const auto& output = batch.outputs[worker];
            if (next[worker] < output.lines.size() && output.lines[next[worker]].first < limit &&
                (earliest == nullptr ||
                 output.lines[next[worker]].first < earliest->lines[*position].first)) {
                earliest = &output;
                position = &next[worker];
            }
        }
        if (earliest == nullptr) {
            break;
        }
        const auto begin = *position == 0 ? 0 : earliest->lines[*position - 1].second;
        const auto end = earliest->lines[*position].second;
        buffer.append(earliest->text, begin, end - begin);
        ++*position;
    }
    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));

    if (failure != nullptr) {
        throw record_error(unit, failure->first, failure->second);
    }
    return events;
}

// Replay with Framing to cut batches, assign to split them, and Worker to apply records
template <typename Framing, typename Worker>
std::size_t run(std::istream& in, std::ostream& out, std::size_t threads,
                std::size_t (*assign)(Batch&, std::size_t), std::string_view unit)
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
            reader.next(batch->text);
            if (batch->text.empty()) {
                reading = false;
                break;
            }
            batch->lines.resize(workers);
            batch->outputs.resize(workers);
            batch->pending = workers;
            next_record = assign(*batch, next_record);
            pipeline.push(std::move(batch));
            ++queued;
        }
        if (queued == 0) {
            return events;
        }
        events += write_batch(*pipeline.pop(), out, buffer, unit);
        --queued;
    }
}

} // namespace

std::size_t replay(std::istream& in, std::ostream& out)
{
    return replay(in, out, std::max(1U, std::thread::hardware_concurrency()));
}

std::size_t replay(std::istream& in, std::ostream& out, std::size_t threads)
{
    return replay(in, out, Format::Csv, threads);
}

std::size_t replay(std::istream& in, std::ostream& out, Format format, std::size_t threads)
{
    if (format == Format::Itch) {
        return run<MessageFraming, ItchWorker>(in, out, threads, assign_messages, "message");
    }
    return run<LineFraming, CsvWorker>(in, out, threads, assign_lines, "line");
}

} // namespace order_book
