#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>
#include <iostream>

#include "WAVM/Emscripten/Emscripten.h"
#include "WAVM/IR/Module.h"
#include "WAVM/IR/Operators.h"
#include "WAVM/IR/Validate.h"
#include "WAVM/Inline/CLI.h"
#include "WAVM/Inline/HashMap.h"
#include "WAVM/Runtime/Linker.h"
#include "WAVM/WASTParse/WASTParse.h"

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::Runtime;

struct RootResolver : Resolver {
    Compartment *compartment;
    HashMap<std::string, ModuleInstance *> moduleNameToInstanceMap;

    RootResolver(Compartment *inCompartment) : compartment(inCompartment) {}

    bool resolve(const std::string &moduleName,
                 const std::string &exportName,
                 ExternType type,
                 Object *&outObject) override {
        auto namedInstance = moduleNameToInstanceMap.get(moduleName);
        if (namedInstance) {
            outObject = getInstanceExport(*namedInstance, exportName);
            if (outObject) {
                if (isA(outObject, type)) { return true; }
                else {
                    std::cout << "Resolved import %s.%s to a %s, but was expecting %s\n" << moduleName.c_str()
                              << exportName.c_str() << asString(getObjectType(outObject)).c_str()
                              << asString(type).c_str();
                    return false;
                }
            }
        }
        outObject = getStubObject(exportName, type);
        return true;
    }

    Object *getStubObject(const std::string &exportName, ExternType type) const {
        // If the import couldn't be resolved, stub it in.
        switch (type.kind) {
            case IR::ExternKind::function: {
                // Generate a function body that just uses the unreachable op to fault if called.
                Serialization::ArrayOutputStream codeStream;
                OperatorEncoderStream encoder(codeStream);
                encoder.unreachable();
                encoder.end();

                // Generate a module for the stub function.
                IR::Module stubIRModule;
                DisassemblyNames stubModuleNames;
                stubIRModule.types.push_back(asFunctionType(type));
                stubIRModule.functions.defs.push_back({{0}, {}, std::move(codeStream.getBytes()), {}});
                stubIRModule.exports.push_back({"importStub", IR::ExternKind::function, 0});
                stubModuleNames.functions.push_back({"importStub: " + exportName, {}, {}});
                IR::setDisassemblyNames(stubIRModule, stubModuleNames);
                IR::validatePreCodeSections(stubIRModule);
                DeferredCodeValidationState deferredCodeValidationState;
                IR::validatePostCodeSections(stubIRModule, deferredCodeValidationState);

                // Instantiate the module and return the stub function instance.
                auto stubModule = compileModule(stubIRModule);
                auto stubModuleInstance = instantiateModule(compartment, stubModule, {}, "importStub");
                return getInstanceExport(stubModuleInstance, "importStub");
            }
            case IR::ExternKind::memory: {
                return asObject(
                        Runtime::createMemory(compartment, asMemoryType(type), std::string(exportName)));
            }
            case IR::ExternKind::table: {
                return asObject(
                        Runtime::createTable(compartment, asTableType(type), std::string(exportName)));
            }
            case IR::ExternKind::global: {
                return asObject(Runtime::createGlobal(
                        compartment,
                        asGlobalType(type),
                        IR::Value(asGlobalType(type).valueType, IR::UntaggedValue())));
            }
            case IR::ExternKind::exceptionType: {
                return asObject(
                        Runtime::createExceptionType(compartment, asExceptionType(type), "importStub"));
            }
            default:
                Errors::unreachable();
        };
    }
};

static bool loadModule(const char *filename, IR::Module &outModule) {
    // Read the specified file into an array.
    std::vector<U8> fileBytes;
    if (!loadFile(filename, fileBytes)) { return false; }

    fileBytes.push_back(0);

    // Load it as a text irModule.
    std::vector<WAST::Error> parseErrors;
    if (!WAST::parseModule(
            (const char *) fileBytes.data(), fileBytes.size(), outModule, parseErrors)) {
        std::cout << "Error parsing WebAssembly text file:\n";
        WAST::reportParseErrors(filename, parseErrors);
        return false;
    }

    return true;
}

struct CommandLineOptions {
    const char *filename = nullptr;
    char **args = nullptr;
};

static int run(const CommandLineOptions &options) {
    IR::Module irModule;

    // Load the module.
    if (!loadModule(options.filename, irModule)) {
        return EXIT_FAILURE;
    }

    // Compile the module.
    Runtime::ModuleRef module = nullptr;
    module = Runtime::compileModule(irModule);

    // Link the module with the intrinsic modules.
    Compartment *compartment = Runtime::createCompartment();
    Context *context = Runtime::createContext(compartment);
    RootResolver rootResolver(compartment);

    Emscripten::Instance *emscriptenInstance = nullptr;
    emscriptenInstance = Emscripten::instantiate(compartment, irModule);
    if (emscriptenInstance) {
        rootResolver.moduleNameToInstanceMap.set("env", emscriptenInstance->env);
        rootResolver.moduleNameToInstanceMap.set("asm2wasm", emscriptenInstance->asm2wasm);
    }

    LinkResult linkResult = linkModule(irModule, rootResolver);
    if (!linkResult.success) {
        std::cout << "Failed to link module:\n";
        for (auto &missingImport : linkResult.missingImports) {
            std::cout << "Missing import: module=\"%s\" export=\"%s\" type=\"%s\"\n" << missingImport.moduleName.c_str()
                      << missingImport.exportName.c_str() << asString(missingImport.type).c_str();
        }
        return EXIT_FAILURE;
    }

    // Instantiate the module.
    ModuleInstance *moduleInstance = instantiateModule(
            compartment, module, std::move(linkResult.resolvedImports), options.filename);
    if (!moduleInstance) { return EXIT_FAILURE; }

    // Call the module start function, if it has one.
    Function *startFunction = getStartFunction(moduleInstance);
    if (startFunction) { invokeFunctionChecked(context, startFunction, {}); }


    // Call the Emscripten global initalizers.
    Emscripten::initializeGlobals(context, irModule, moduleInstance);


    // Look up the function export to call.
    Function *function;

    function = asFunctionNullable(getInstanceExport(moduleInstance, "main"));
    if (!function) {
        function = asFunctionNullable(getInstanceExport(moduleInstance, "_main"));
    }
    if (!function) {
        std::cout << "Module does not export main function";
        return EXIT_FAILURE;
    }

    FunctionType functionType = getFunctionType(function);

    // Set up the arguments for the invoke.
    std::vector<Value> invokeArgs;
    if (functionType.params().size() == 2) {
        std::vector<const char *> argStrings;
        argStrings.push_back(options.filename);
        char **args = options.args;
        while (*args) { argStrings.push_back(*args++); };

        wavmAssert(emscriptenInstance);
        Emscripten::injectCommandArgs(emscriptenInstance, argStrings, invokeArgs);
    } else if (functionType.params().size() > 0) {
        std::cout << "WebAssembly function requires " << PRIu64 << " argument(s), but only 0 or 2 can be passed!"
                  << functionType.params().size();
        return EXIT_FAILURE;
    }

    IR::ValueTuple functionResults = invokeFunctionChecked(context, function, invokeArgs);

    if (functionResults.size() == 1 && functionResults[0].type == ValueType::i32) {
        return functionResults[0].i32;
    } else {
        return EXIT_SUCCESS;
    }
}

static void showHelp() {
    std::cout << "Usage: wavm-run [programfile] [--] [arguments]\n"
                 "  -h|--help             Display this message\n";
}

int main(int argc, char **argv) {
    CommandLineOptions options;
    options.args = argv;
    while (*++options.args) {
        if (!strcmp(*options.args, "--help") || !strcmp(*options.args, "-h")) {
            showHelp();
            return EXIT_SUCCESS;
        } else if (!options.filename) {
            options.filename = *options.args;
        } else {
            break;
        }
    }

    if (!options.filename) {
        showHelp();
        return EXIT_FAILURE;
    }

    // Treat any unhandled exception (e.g. in a thread) as a fatal error.
    Runtime::setUnhandledExceptionHandler([](Runtime::Exception &&exception) {
        Errors::fatalf("Runtime exception: %s", describeException(exception).c_str());
    });

    return run(options);
}
