#include "generator.hpp"

#include <google/protobuf/compiler/plugin.h>

int main(int argc, char* argv[]) {
    fletcher::ArrowRowGenerator generator;
    return google::protobuf::compiler::PluginMain(argc, argv, &generator);
}
