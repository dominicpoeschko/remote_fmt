#include <type_traits>
#include <remote_fmt/remote_fmt.hpp>
#include <remote_fmt/parser.hpp>
#include <remote_fmt/type_identifier.hpp>
#include <remote_fmt/catalog_helpers.hpp>

#include <span>
#include <cstddef>
#include <map>
#include <vector>
#include <fmt/format.h>

//CommunicationBackend class provides an interface between the
//remote_fmt printer and the communication channel (Socket/UART/etc).
//In this example the communication channel is abstracted by a std::vector.
struct CommunicationBackend{
    std::vector<std::byte> memory;

    void initTransfer(){
        fmt::print("Before write\n");
    }
    void finalizeTransfer(){
        fmt::print("After write\n");
    }
    void write(std::span<std::byte const> s){
        fmt::print("Write {}\n", s.size());
        memory.insert(memory.end(), s.begin(), s.end());
    }
};

int main(){
    using namespace sc::literals; 
    //The remote_fmt printer is instanced with the CommunicationBackend class
    //as a template parameter.
    remote_fmt::Printer<CommunicationBackend> printer{};
    
    //The print-function is called and the CommunicationBackend handles
    //communication with the remote device.
    printer.print("Test {}"_sc, 123);
    assert(!printer.get_com_backend().memory.empty());
    
    //The data is sent to the input buffer of the remote device.
    auto const& buffer = printer.get_com_backend().memory;

    //The remote device parses the data from the buffer with a catalog from the json file
    auto const catalog = 
        remote_fmt::parseStringConstantsFromJsonFile("catalog_generator_string_constant.json");
    auto const& [message, remainingBytes, discardedBytes] = remote_fmt::parse(std::span{buffer}, catalog);
    
    assert(remainingBytes.size() == 0);
    assert(discardedBytes == 0);
    assert(message.has_value());
    assert(message.value() == "Test 123");
    return 0;
}