# Order book

Given a CSV file comprising of a day of market events from an equity exchange
the program builds an order book.

An order book comprises two ordered lists of price levels -- one for the bid
side, and another for asks. A price level represents a FIFO queue of orders
submitted at that price.

## Usage

The program expects one argument which is a filename to input CSV file and
prints to output the best bid and ask price and quantity whenever any of them
changes in the following format:
```
<TIME>,<TICKER>,<BBP>,<BBQ>,<BAP>,<BAQ>
```
where here `<TIME>` is number of milliseconds since the start of the trading
day, `<TICKER>` is the ticker for which update was done, `<BBP>` and `<BBQ>`
are price and quantity of the best bid level or empty if there are no bids, and
similarly `<BAP>` and `<BAQ>` for the ask.

### Usage example

```bash
order-book testdata.csv
```

## Build instructions

The project is using [CMake](https://cmake.org) for build and
[Google Test](https://github.com/google/googletest) for unit-testng.

###  Without unit tests

```bash
mkdir build && cd build
cmake ..
make
```

###  With unit tests

```bash
git clone https://github.com/google/googletest.git external/googletest
mkdir build && cd build
cmake -DBUILD_TESTS=ON ..
make
make CTEST_OUTPUT_ON_FAILURE=1 test
```
