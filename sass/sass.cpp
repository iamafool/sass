﻿#include <iostream>
#include <string>
#include "Lexer.h"
#include "Parser.h"
#include "Interpreter.h"
#include "DataEnvironment.h"
#include "spdlog/spdlog.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

int main(int argc, char** argv) {
    std::string sasFile;
    std::string logFile;
    std::string lstFile;

    // Parse command line arguments
    // Expected: -sas=xxx.sas -log=xxx.log -lst=xxx.lst
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("-sas=", 0) == 0) {
            sasFile = arg.substr(5);
        }
        else if (arg.rfind("-log=", 0) == 0) {
            logFile = arg.substr(5);
        }
        else if (arg.rfind("-lst=", 0) == 0) {
            lstFile = arg.substr(5);
        }
    }

    bool batchMode = !sasFile.empty() && !logFile.empty() && !lstFile.empty();

    // Create two loggers: one for SAS log (logLogger), one for SAS listing output (lstLogger).
    std::shared_ptr<spdlog::logger> logLogger;
    std::shared_ptr<spdlog::logger> lstLogger;

    if (batchMode) {
        // Batch mode: log and output to files
        // Overwrite files if they exist: set truncate = true
        auto logSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile, true);
        auto lstSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(lstFile, true);

        logLogger = std::make_shared<spdlog::logger>("log", logSink);
        lstLogger = std::make_shared<spdlog::logger>("lst", lstSink);
    }
    else {
        // Interactive mode: log and output to console
        // Use different color sinks for distinction if you like
        auto logSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto lstSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        logLogger = std::make_shared<spdlog::logger>("log", logSink);
        lstLogger = std::make_shared<spdlog::logger>("lst", lstSink);
    }

    // Register loggers so they can be retrieved globally if needed
    spdlog::register_logger(logLogger);
    spdlog::register_logger(lstLogger);

    // Set default levels, could make this configurable
    logLogger->set_level(spdlog::level::info);
    lstLogger->set_level(spdlog::level::info);

    // Example usage:
    // Write to log
    logLogger->info("SAS Interpreter started.");
    if (batchMode) {
        logLogger->info("Running in batch mode with SAS file: {}", sasFile);
    }
    else {
        logLogger->info("Running in interactive mode.");
    }

    // Simulate reading and processing SAS code
    // logLogger->debug("Parsing DATA step...");
    // ...

    // Write to output (lst)
    lstLogger->info("SAS results output:");
    lstLogger->info("OBS     VAR1     VAR2");
    lstLogger->info("1       10       20");
    lstLogger->info("2       30       40");
    // ...

    logLogger->info("SAS Interpreter finished.");
    // Example SAS-like code:
    // data out; set in;
    // x = 42;
    // if x then output;
    // run;

    std::string code = "data out; set in; x = 42; if x then output; run;";

    // Prepare environment
    DataEnvironment env;
    // Create a sample input dataset
    DataSet inData;
    inData.rows.push_back({ {{"x", 0.0}} });
    inData.rows.push_back({ {{"x", 1.0}} });
    env.dataSets["in"] = inData;

    // Lex
    Lexer lexer(code);
    std::vector<Token> tokens;
    Token tok;
    while ((tok = lexer.getNextToken()).type != TokenType::EOF_TOKEN) {
        tokens.push_back(tok);
    }

    // Parse
    Parser parser(tokens);
    std::unique_ptr<ASTNode> root = parser.parse();

    // Interpret
    Interpreter interpreter(env);
    interpreter.execute(root);

    return 0;
}
