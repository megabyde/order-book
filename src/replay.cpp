#include <order_book/feed.hpp>
#include <order_book/order_book.hpp>
#include <order_book/replay.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
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
#include <vector>

namespace order_book {

namespace {

// Workers receive the input in batches of whole lines of at least this size
constexpr std::size_t batch_size = std::size_t{1} << 20U;
// Batches read ahead of the one being written out
constexpr std::size_t batches_in_flight = 4;

void append_uint(std::string& buffer, std::uint64_t value)
{
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 1> digits{};
    auto* const end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    buffer.append(digits.data(), end);
}

void append_level(std::string& buffer, const std::optional<Level>& level)
{
    if (level) {
        append_uint(buffer, level->price);
        buffer += ',';
        append_uint(buffer, level->quantity);
    }
    else {
        buffer += ',';
    }
}

std::string describe(BookError error, std::string_view order)
{
    return std::format("{} order '{}'",
                       error == BookError::DuplicateOrder ? "duplicate" : "unknown", order);
}

std::invalid_argument line_error(std::size_t number, std::string_view message)
{
    const auto what = std::format("line {}: {}", number, message);
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

// Cuts a stream into batches of whole lines
class BatchReader {
public:
    explicit BatchReader(std::istream& in) : m_source(in.rdbuf()), m_done(!in) {}

    // At least batch_size bytes of LF-terminated lines, fewer at the end of the stream, where the
    // last line may lack its LF. Empty once the stream is exhausted.
    std::string next()
    {
        auto text = std::exchange(m_carry, {});
        auto last_newline = std::string::npos;
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
            if (const auto found = std::string_view(text).substr(size).rfind('\n');
                found != std::string_view::npos) {
                last_newline = size + found;
            }
            if (text.size() >= batch_size && last_newline != std::string::npos) {
                m_carry.assign(text, last_newline + 1);
                text.resize(last_newline + 1);
                break;
            }
        }
        return text;
    }

private:
    std::streambuf* m_source;
    bool m_done;
    // Start of a line that continues into the next batch
    std::string m_carry;
};

// What one worker made of one batch
struct Output {
    std::string text;
    // Line number of each line of text, and the offset just past it
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    std::size_t events = 0;
    // Number of the first line that could not be parsed or applied, and why
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
    // Each worker's lines, in feed order, viewing text
    std::vector<std::vector<Line>> lines;
    std::vector<Output> outputs;
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

// Keeps the books of the tickers whose lines it is given
class Worker {
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

        const auto before = book->second.bbo();
        if (const auto applied = book->second.apply(message->order, message->event); !applied) {
            output.failure.emplace(number, describe(applied.error(), message->order));
            return false;
        }
        if (const auto after = book->second.bbo(); after != before) {
            auto& text = output.text;
            append_uint(text, message->time);
            text += ',';
            text += message->ticker;
            text += ',';
            append_level(text, after.bid);
            text += ',';
            append_level(text, after.ask);
            text += '\n';
            output.lines.emplace_back(number, text.size());
        }
        ++output.events;
        return true;
    }

    std::unordered_map<std::string, OrderBook, StringHash, std::equal_to<>> m_books;
};

// Runs the workers over a queue of batches, which every worker processes in order
class Pipeline {
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

// Write the batch's output in line order and return its event count. Throws for the batch's first
// failing line after writing the output before it.
std::size_t write_batch(Batch& batch, std::ostream& out, std::string& buffer)
{
    const std::pair<std::size_t, std::string>* failure = nullptr;
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
        throw line_error(failure->first, failure->second);
    }
    return events;
}

} // namespace

std::size_t replay(std::istream& in, std::ostream& out)
{
    return replay(in, out, std::max(1U, std::thread::hardware_concurrency()));
}

std::size_t replay(std::istream& in, std::ostream& out, std::size_t threads)
{
    const auto workers = std::max<std::size_t>(threads, 1);
    BatchReader reader(in);
    Pipeline pipeline(workers);
    std::string buffer;
    std::size_t next_line = 1;
    std::size_t queued = 0;
    bool reading = true;
    std::size_t events = 0;
    while (true) {
        while (reading && queued < batches_in_flight) {
            auto text = reader.next();
            if (text.empty()) {
                reading = false;
                break;
            }
            auto batch = std::make_shared<Batch>();
            batch->text = std::move(text);
            batch->lines.resize(workers);
            batch->outputs.resize(workers);
            batch->pending = workers;
            next_line = assign_lines(*batch, next_line);
            pipeline.push(std::move(batch));
            ++queued;
        }
        if (queued == 0) {
            return events;
        }
        events += write_batch(*pipeline.pop(), out, buffer);
        --queued;
    }
}

} // namespace order_book
