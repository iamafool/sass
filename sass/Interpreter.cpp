#include "Interpreter.h"
#include "Sorter.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <unordered_set>
#include <numeric>

// Execute the entire program
void Interpreter::executeProgram(const std::unique_ptr<ProgramNode> &program) {
    for (const auto &stmt : program->statements) {
        try {
            execute(stmt.get());
        }
        catch (const std::runtime_error &e) {
            logLogger.error("Execution error: {}", e.what());
            // Continue with the next statement
        }
    }
}

// Execute a single AST node
void Interpreter::execute(ASTNode *node) {
    if (auto ds = dynamic_cast<DataStepNode*>(node)) {
        executeDataStep(ds);
    }
    else if (auto opt = dynamic_cast<OptionsNode*>(node)) {
        executeOptions(opt);
    }
    else if (auto lib = dynamic_cast<LibnameNode*>(node)) {
        executeLibname(lib);
    }
    else if (auto title = dynamic_cast<TitleNode*>(node)) {
        executeTitle(title);
    }
    else if (auto proc = dynamic_cast<ProcNode*>(node)) {
        executeProc(proc);
    }
    else if (auto ifElseIf = dynamic_cast<IfElseIfNode*>(node)) {
        executeIfElse(ifElseIf);
    }
    else if (auto arrayNode = dynamic_cast<ArrayNode*>(node)) {
        executeArray(arrayNode);
    }
    else if (auto mergeNode = dynamic_cast<MergeStatementNode*>(node)) {
        executeMerge(mergeNode);
    }
    else if (auto byNode = dynamic_cast<ByStatementNode*>(node)) {
        executeBy(byNode);
    }
    else if (auto doLoop = dynamic_cast<DoLoopNode*>(node)) {
        executeDoLoop(doLoop);
    }
    else if (auto endNode = dynamic_cast<EndNode*>(node)) {
        executeEnd(endNode);
    }
    else {
        // Handle other statements
        // For now, ignore unknown statements or throw an error
        throw std::runtime_error("Unknown AST node encountered during execution.");
    }
}

// Execute a DATA step
void Interpreter::executeDataStep(DataStepNode *node) {
    logLogger.info("Executing DATA step: data {}; set {};", node->outputDataSet, node->inputDataSet);

    // Resolve output dataset name
    std::string outputLibref, outputDataset;
    size_t dotPos = node->outputDataSet.find('.');
    if (dotPos != std::string::npos) {
        outputLibref = node->outputDataSet.substr(0, dotPos);
        outputDataset = node->outputDataSet.substr(dotPos + 1);
    }
    else {
        outputDataset = node->outputDataSet;
    }

    // Resolve input dataset name
    std::string inputLibref, inputDataset;
    dotPos = node->inputDataSet.find('.');
    if (dotPos != std::string::npos) {
        inputLibref = node->inputDataSet.substr(0, dotPos);
        inputDataset = node->inputDataSet.substr(dotPos + 1);
    }
    else {
        inputDataset = node->inputDataSet;
    }

    // Check if input dataset exists
    std::shared_ptr<DataSet> input = nullptr;
    try {
        input = env.getOrCreateDataset(inputLibref, inputDataset);
    }
    catch (const std::runtime_error &e) {
        logLogger.error(e.what());
        return;
    }

    // Create or get the output dataset
    std::shared_ptr<DataSet> output;
    try {
        output = env.getOrCreateDataset(outputLibref, outputDataset);
        output->name = node->outputDataSet;
    }
    catch (const std::runtime_error &e) {
        logLogger.error(e.what());
        return;
    }

    // Log dataset sizes
    logLogger.info("Input dataset '{}' has {} observations.", node->inputDataSet, input->rows.size());
    logLogger.info("Output dataset '{}' will store results.", node->outputDataSet);

    // Variables to control variable retention
    std::vector<std::string> dropVars;
    std::vector<std::string> keepVars;
    std::vector<std::string> retainVars;

    bool hasDrop = false;
    bool hasKeep = false;
    bool hasRetain = false;

    // Check if a MERGE statement exists in the data step
    bool hasMerge = false;
    MergeStatementNode* mergeNode = nullptr;
    for (const auto& stmt : node->statements) {
        if (auto m = dynamic_cast<MergeStatementNode*>(stmt.get())) {
            hasMerge = true;
            mergeNode = m;
            break;
        }
    }

    if (hasMerge && mergeNode) {
        // Handle MERGE logic
        executeMerge(mergeNode);
        // Remove MERGE statement from the data step to avoid re-processing
        // Implement this as per your data structure
    }

    // Execute each row in the input dataset
    for (const auto &row : input->rows) {
        env.currentRow = row; // Set the current row for processing

        // Apply RETAIN variables: retain their values across iterations
        if (hasRetain) {
            for (const auto& var : retainVars) {
                if (env.variables.find(var) != env.variables.end()) {
                    env.currentRow.columns[var] = env.variables[var];
                }
            }
        }

        // Flag to determine if the row should be output
        bool shouldOutput = false;

        // Temporary variables to track DROP/KEEP
        dropVars.clear();
        keepVars.clear();
        retainVars.clear();
        hasDrop = false;
        hasKeep = false;
        hasRetain = false;

        // Execute each statement in the DATA step
        for (const auto &stmt : node->statements) {
            if (auto assign = dynamic_cast<AssignmentNode*>(stmt.get())) {
                executeAssignment(assign);
            }
            else if (auto ifThen = dynamic_cast<IfThenNode*>(stmt.get())) {
                executeIfThen(ifThen);
            }
            else if (auto out = dynamic_cast<OutputNode*>(stmt.get())) {
                executeOutput(out);
                shouldOutput = true;
            }
            else if (auto drop = dynamic_cast<DropNode*>(stmt.get())) {
                executeDrop(drop);
                hasDrop = true;
            }
            else if (auto keep = dynamic_cast<KeepNode*>(stmt.get())) {
                executeKeep(keep);
                hasKeep = true;
            }
            else if (auto retain = dynamic_cast<RetainNode*>(stmt.get())) {
                executeRetain(retain);
                hasRetain = true;
            }
            else if (auto array = dynamic_cast<ArrayNode*>(stmt.get())) {
                executeArray(array);
            }
            else if (auto doNode = dynamic_cast<DoNode*>(stmt.get())) {
                executeDo(doNode);
            }
            else {
                // Handle other DATA step statements if needed
                throw std::runtime_error("Unsupported statement in DATA step.");
            }
        }

        // Apply DROP and KEEP rules
        if (hasDrop && hasKeep) {
            // In SAS, if both DROP and KEEP are specified, KEEP takes precedence
            // Keep only the variables specified in KEEP
            for (auto it = env.currentRow.columns.begin(); it != env.currentRow.columns.end(); ) {
                if (std::find(keepVars.begin(), keepVars.end(), it->first) == keepVars.end()) {
                    it = env.currentRow.columns.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        else if (hasKeep) {
            // Keep only the variables specified in KEEP
            for (auto it = env.currentRow.columns.begin(); it != env.currentRow.columns.end(); ) {
                if (std::find(keepVars.begin(), keepVars.end(), it->first) == keepVars.end()) {
                    it = env.currentRow.columns.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        else if (hasDrop) {
            // Drop the variables specified in DROP
            for (const auto& var : dropVars) {
                env.currentRow.columns.erase(var);
            }
        }

        // Apply RETAIN variables: store their values for the next iteration
        if (hasRetain) {
            for (const auto& var : retainVars) {
                if (env.currentRow.columns.find(var) != env.currentRow.columns.end()) {
                    env.variables[var] = env.currentRow.columns[var];
                }
                else {
                    // If variable not present, retain existing value
                    if (env.variables.find(var) != env.variables.end()) {
                        env.currentRow.columns[var] = env.variables[var];
                    }
                }
            }
        }

        // If 'OUTPUT' was called, add the current row to the output dataset
        if (shouldOutput) {
            output->addRow(env.currentRow);
            logLogger.info("Row outputted to '{}'.", node->outputDataSet);
        }
    }

    logLogger.info("DATA step '{}' executed successfully. Output dataset has {} observations.",
                   node->outputDataSet, output->rows.size());

    // For demonstration, print the output dataset
    lstLogger.info("SAS Results (Dataset: {}):", node->outputDataSet);
    if (!env.title.empty()) {
        lstLogger.info("Title: {}", env.title);
    }
 
    // Print column headers
    std::string header;
    for (size_t i = 0; i < output->columnOrder.size(); ++i) {
        header += output->columnOrder[i];
        if (i < output->columnOrder.size() - 1) header += "\t";
    }
    lstLogger.info("{}", header);

    // Print rows
    int obs = 1;
    for (const auto &row : output->rows) {
        std::string rowStr = std::to_string(obs++) + "\t";
        for (size_t i = 0; i < output->columnOrder.size(); ++i) {
            const std::string &col = output->columnOrder[i];
            auto it = row.columns.find(col);
            if (it != row.columns.end()) {
                rowStr += toString(it->second);
            }
            else {
                rowStr += ".";
            }
            if (i < output->columnOrder.size() - 1) rowStr += "\t";
        }
        lstLogger.info("{}", rowStr);
    }
}

// Execute an assignment statement
void Interpreter::executeAssignment(AssignmentNode *node) {
    Value val = evaluate(node->expression.get());
    env.setVariable(node->varName, val);
    logLogger.info("Assigned {} = {}", node->varName, toString(val));
}

// Execute an IF-THEN statement
void Interpreter::executeIfThen(IfThenNode *node) {
    Value cond = evaluate(node->condition.get());
    double d = toNumber(cond);
    logLogger.info("Evaluating IF condition: {}", d);

    if (d != 0.0) { // Non-zero is true
        for (const auto &stmt : node->thenStatements) {
            if (auto assign = dynamic_cast<AssignmentNode*>(stmt.get())) {
                executeAssignment(assign);
            }
            else if (auto out = dynamic_cast<OutputNode*>(stmt.get())) {
                executeOutput(out);
            }
            else {
                throw std::runtime_error("Unsupported statement in IF-THEN block.");
            }
        }
    }
}

// Execute an OUTPUT statement
void Interpreter::executeOutput(OutputNode *node) {
    // In this implementation, 'OUTPUT' sets a flag in the DATA step execution to add the current row
    // The actual addition to the dataset is handled in 'executeDataStep'
    // However, to make this explicit, you can modify 'currentRow' if needed
    logLogger.info("OUTPUT statement executed. Current row will be added to the output dataset.");
    // Optionally, set a flag or manipulate 'currentRow' here
}

// Execute an OPTIONS statement
void Interpreter::executeOptions(OptionsNode* node) {
    for (const auto& opt : node->options) {
        env.setOption(opt.first, opt.second);
        logLogger.info("Set option {} = {}", opt.first, opt.second);
    }
}

// Execute a LIBNAME statement
void Interpreter::executeLibname(LibnameNode* node) {
    env.setLibref(node->libref, node->path);
    logLogger.info("Libname assigned: {} = '{}'", node->libref, node->path);

    // Load multiple datasets if required
    // For demonstration, let's load 'in.csv' as 'mylib.in'
    std::string csvPath = node->path + "\\" + "in.csv"; // Adjust path separator as needed
    try {
        env.loadDatasetFromCSV(node->libref, "in", csvPath);
        logLogger.info("Loaded dataset '{}' from '{}'", node->libref + ".in", csvPath);
    }
    catch (const std::runtime_error &e) {
        logLogger.error("Failed to load dataset: {}", e.what());
    }
}

// Execute a TITLE statement
void Interpreter::executeTitle(TitleNode* node) {
    env.setTitle(node->title);
    logLogger.info("Title set to: '{}'", node->title);
    lstLogger.info("Title: {}", env.title);
}

// Execute a PROC step
void Interpreter::executeProc(ProcNode* node) {
    if (auto procSort = dynamic_cast<ProcSortNode*>(node)) {
        executeProcSort(procSort);
    }
    else {
        if (node->procName == "print") {
            logLogger.info("Executing PROC PRINT on dataset '{}'.", node->datasetName);
            try {
                auto dataset = env.getOrCreateDataset("", node->datasetName);
                lstLogger.info("PROC PRINT Results for Dataset '{}':", dataset->name);
                if (!env.title.empty()) {
                    lstLogger.info("Title: {}", env.title);
                }

                // Print column headers
                std::string header;
                for (size_t i = 0; i < dataset->columnOrder.size(); ++i) {
                    header += dataset->columnOrder[i];
                    if (i < dataset->columnOrder.size() - 1) header += "\t";
                }
                lstLogger.info("{}", header);

                // Print rows
                int obs = 1;
                for (const auto& row : dataset->rows) {
                    std::string rowStr = std::to_string(obs++) + "\t";
                    for (size_t i = 0; i < dataset->columnOrder.size(); ++i) {
                        const std::string& col = dataset->columnOrder[i];
                        auto it = row.columns.find(col);
                        if (it != row.columns.end()) {
                            rowStr += toString(it->second);
                        }
                        else {
                            rowStr += ".";
                        }
                        if (i < dataset->columnOrder.size() - 1) rowStr += "\t";
                    }
                    lstLogger.info("{}", rowStr);
                }
            }
            catch (const std::runtime_error& e) {
                logLogger.error("PROC PRINT failed: {}", e.what());
            }
        }
        else if (node->procName == "means") {
            // Should not reach here; PROC MEANS is handled as ProcMeansNode
            throw std::runtime_error("PROC MEANS should be handled as ProcMeansNode");
        }
        else {
            logLogger.error("Unsupported PROC: {}", node->procName);
        }
    }
}

// Convert Value to number (double)
double Interpreter::toNumber(const Value &v) {
    if (std::holds_alternative<double>(v)) {
        return std::get<double>(v);
    }
    else if (std::holds_alternative<std::string>(v)) {
        try {
            return std::stod(std::get<std::string>(v));
        }
        catch (...) {
            return 0.0; // Represent missing as 0.0 or handle differently
        }
    }
    return 0.0;
}

// Convert Value to string
std::string Interpreter::toString(const Value &v) {
    if (std::holds_alternative<double>(v)) {
        // Remove trailing zeros for cleaner output
        std::string numStr = std::to_string(std::get<double>(v));
        numStr.erase(numStr.find_last_not_of('0') + 1, std::string::npos);
        if (numStr.back() == '.') numStr.pop_back();
        return numStr;
    }
    else {
        return std::get<std::string>(v);
    }
}

Value Interpreter::getVariable(const std::string& varName) const {
    return env.getVariable(varName);
}

void Interpreter::setVariable(const std::string& varName, const Value& val) {
    env.setVariable(varName, val);
}

// Evaluate an expression node
Value Interpreter::evaluate(ASTNode *node) {
    if (auto numNode = dynamic_cast<NumberNode*>(node)) {
        return numNode->value;
    }
    else if (auto strNode = dynamic_cast<StringNode*>(node)) {
        return strNode->value;
    }
    else if (auto var = dynamic_cast<VariableNode*>(node)) {
        auto it = env.variables.find(var->varName);
        if (it != env.variables.end()) {
            return it->second;
        }
        else {
            // Variable not found, return missing value
            logLogger.warn("Variable '{}' not found. Using missing value.", var->varName);
            return std::nan("");
        }
    }
    else if (auto funcCall = dynamic_cast<FunctionCallNode*>(node)) {
        return evaluateFunctionCall(funcCall);
    }
    else if (auto arrayElem = dynamic_cast<ArrayElementNode*>(node)) {
        int index = static_cast<int>(toNumber(evaluate(arrayElem->index.get())));
        return getArrayElement(arrayElem->arrayName, index);
    }
    else if (auto bin = dynamic_cast<BinaryOpNode*>(node)) {
        Value leftVal = evaluate(bin->left.get());
        Value rightVal = evaluate(bin->right.get());
        std::string op = bin->op;

        double l = toNumber(leftVal);
        double r = toNumber(rightVal);

        if (op == "+") return l + r;
        else if (op == "-") return l - r;
        else if (op == "*") return l * r;
        else if (op == "/") return (r != 0.0) ? l / r : std::nan("");
        else if (op == ">") return (l > r) ? 1.0 : 0.0;
        else if (op == "<") return (l < r) ? 1.0 : 0.0;
        else if (op == ">=") return (l >= r) ? 1.0 : 0.0;
        else if (op == "<=") return (l <= r) ? 1.0 : 0.0;
        else if (op == "==") return (l == r) ? 1.0 : 0.0;
        else if (op == "!=") return (l != r) ? 1.0 : 0.0;
        else if (op == "and") return ((l != 0.0) && (r != 0.0)) ? 1.0 : 0.0;
        else if (op == "or") return ((l != 0.0) || (r != 0.0)) ? 1.0 : 0.0;
        else {
            throw std::runtime_error("Unsupported binary operator: " + op);
        }
    }
    // Handle more expression types as needed
    throw std::runtime_error("Unsupported expression type during evaluation.");
}

// Execute a DROP statement
void Interpreter::executeDrop(DropNode* node) {
    for (const auto& var : node->variables) {
        env.currentRow.columns.erase(var);
        logLogger.info("Dropped variable '{}'.", var);
    }
}

// Execute a KEEP statement
void Interpreter::executeKeep(KeepNode* node) {
    // Retain only the specified variables
    std::vector<std::string> currentVars;
    for (const auto& varPair : env.currentRow.columns) {
        currentVars.push_back(varPair.first);
    }

    for (const auto& var : currentVars) {
        if (std::find(node->variables.begin(), node->variables.end(), var) == node->variables.end()) {
            env.currentRow.columns.erase(var);
            logLogger.info("Kept variable '{}'; other variables dropped.", var);
        }
    }
}

// Execute a RETAIN statement
void Interpreter::executeRetain(RetainNode* node) {
    for (const auto& var : node->variables) {
        retainVars.push_back(var);
        logLogger.info("Retained variable '{}'.", var);
    }
}

// Execute an ARRAY statement
void Interpreter::executeArray(ArrayNode* node) {
    logLogger.info("Executing ARRAY declaration: {}", node->arrayName);
    // Validate array size
    if (node->size != static_cast<int>(node->variables.size())) {
        throw std::runtime_error("Array size does not match the number of variables.");
    }

    // Store the array in the interpreter's array map
    arrays[node->arrayName] = node->variables;

    logLogger.info("Array '{}' with size {} and variables: {}", node->arrayName, node->size,
        [&]() -> std::string {
            std::string vars;
            for (const auto& var : node->variables) {
                vars += var + " ";
            }
            return vars;
        }());
}

Value Interpreter::getArrayElement(const std::string& arrayName, int index) {
    if (arrays.find(arrayName) == arrays.end()) {
        throw std::runtime_error("Undefined array: " + arrayName);
    }
    if (index < 1 || index > static_cast<int>(arrays[arrayName].size())) {
        throw std::runtime_error("Array index out of bounds for array: " + arrayName);
    }
    std::string varName = arrays[arrayName][index - 1];
    auto it = env.currentRow.columns.find(varName);
    if (it != env.currentRow.columns.end()) {
        return it->second;
    }
    else {
        // Variable not found, assume missing value represented as 0 or empty string
        return 0.0; // or throw an error based on SAS behavior
    }
}

void Interpreter::setArrayElement(const std::string& arrayName, int index, const Value& value) {
    if (arrays.find(arrayName) == arrays.end()) {
        throw std::runtime_error("Undefined array: " + arrayName);
    }
    if (index < 1 || index > static_cast<int>(arrays[arrayName].size())) {
        throw std::runtime_error("Array index out of bounds for array: " + arrayName);
    }
    std::string varName = arrays[arrayName][index - 1];
    env.currentRow.columns[varName] = value;
}


// Execute a DO loop
void Interpreter::executeDo(DoNode* node) {
    // Evaluate start and end expressions
    Value startVal = evaluate(node->startExpr.get());
    Value endVal = evaluate(node->endExpr.get());
    double start = toNumber(startVal);
    double end = toNumber(endVal);
    double increment = 1.0; // Default increment

    if (node->incrementExpr) {
        Value incVal = evaluate(node->incrementExpr.get());
        increment = toNumber(incVal);
    }

    logLogger.info("Starting DO loop: {} = {} to {} by {}", node->loopVar, start, end, increment);

    // Initialize loop variable
    env.currentRow.columns[node->loopVar] = start;

    // Loop
    if (increment > 0) {
        while (env.currentRow.columns[node->loopVar].index() == 0 && std::get<double>(env.currentRow.columns[node->loopVar]) <= end) {
            // Execute loop statements
            for (const auto& stmt : node->statements) {
                if (auto assign = dynamic_cast<AssignmentNode*>(stmt.get())) {
                    executeAssignment(assign);
                }
                else if (auto ifThen = dynamic_cast<IfThenNode*>(stmt.get())) {
                    executeIfThen(ifThen);
                }
                else if (auto out = dynamic_cast<OutputNode*>(stmt.get())) {
                    executeOutput(out);
                }
                else if (auto drop = dynamic_cast<DropNode*>(stmt.get())) {
                    executeDrop(drop);
                }
                else if (auto keep = dynamic_cast<KeepNode*>(stmt.get())) {
                    executeKeep(keep);
                }
                else if (auto retain = dynamic_cast<RetainNode*>(stmt.get())) {
                    executeRetain(retain);
                }
                else if (auto array = dynamic_cast<ArrayNode*>(stmt.get())) {
                    executeArray(array);
                }
                else if (auto doNode = dynamic_cast<DoNode*>(stmt.get())) {
                    executeDo(doNode);
                }
                else {
                    // Handle other DATA step statements if needed
                    throw std::runtime_error("Unsupported statement in DO loop.");
                }
            }

            // Increment loop variable
            double currentVal = toNumber(env.currentRow.columns[node->loopVar]);
            currentVal += increment;
            env.currentRow.columns[node->loopVar] = currentVal;
        }
    }
    else if (increment < 0) {
        while (env.currentRow.columns[node->loopVar].index() == 0 && std::get<double>(env.currentRow.columns[node->loopVar]) >= end) {
            // Execute loop statements
            for (const auto& stmt : node->statements) {
                if (auto assign = dynamic_cast<AssignmentNode*>(stmt.get())) {
                    executeAssignment(assign);
                }
                else if (auto ifThen = dynamic_cast<IfThenNode*>(stmt.get())) {
                    executeIfThen(ifThen);
                }
                else if (auto out = dynamic_cast<OutputNode*>(stmt.get())) {
                    executeOutput(out);
                }
                else if (auto drop = dynamic_cast<DropNode*>(stmt.get())) {
                    executeDrop(drop);
                }
                else if (auto keep = dynamic_cast<KeepNode*>(stmt.get())) {
                    executeKeep(keep);
                }
                else if (auto retain = dynamic_cast<RetainNode*>(stmt.get())) {
                    executeRetain(retain);
                }
                else if (auto array = dynamic_cast<ArrayNode*>(stmt.get())) {
                    executeArray(array);
                }
                else if (auto doNode = dynamic_cast<DoNode*>(stmt.get())) {
                    executeDo(doNode);
                }
                else {
                    // Handle other DATA step statements if needed
                    throw std::runtime_error("Unsupported statement in DO loop.");
                }
            }

            // Increment loop variable
            double currentVal = toNumber(env.currentRow.columns[node->loopVar]);
            currentVal += increment;
            env.currentRow.columns[node->loopVar] = currentVal;
        }
    }
    else {
        throw std::runtime_error("DO loop increment cannot be zero.");
    }

    logLogger.info("Completed DO loop: {} reached {}", node->loopVar, env.currentRow.columns[node->loopVar].index() == 0 ? toString(env.currentRow.columns[node->loopVar]) : "unknown");
}

void Interpreter::executeProcSort(ProcSortNode* node) {
    logLogger.info("Executing PROC SORT");

    // Retrieve the input dataset
    DataSet* inputDS = env.getOrCreateDataset(node->inputDataSet, node->inputDataSet).get();
    if (!inputDS) {
        throw std::runtime_error("Input dataset '" + node->inputDataSet + "' not found for PROC SORT.");
    }

    // Apply WHERE condition if specified
    DataSet* filteredDS = inputDS;
    if (node->whereCondition) {
        // Create a temporary dataset to hold filtered rows
        std::string tempDSName = "TEMP_SORT_FILTERED";
        auto tempDS = env.getOrCreateDataset(tempDSName, tempDSName);
        tempDS->rows.clear();

        for (const auto& row : inputDS->rows) {
            env.currentRow = row;
            Value condValue = evaluate(node->whereCondition.get());
            bool conditionTrue = false;
            if (std::holds_alternative<double>(condValue)) {
                conditionTrue = (std::get<double>(condValue) != 0.0);
            }
            else if (std::holds_alternative<std::string>(condValue)) {
                conditionTrue = (!std::get<std::string>(condValue).empty());
            }
            // Add other data types as needed

            if (conditionTrue) {
                tempDS->rows.push_back(row);
            }
        }

        filteredDS = tempDS.get();
        logLogger.info("Applied WHERE condition. {} observations remain after filtering.", filteredDS->rows.size());
    }

    // Sort the filtered dataset by BY variables
    Sorter::sortDataset(filteredDS, node->byVariables);
    logLogger.info("Sorted dataset '{}' by variables: {}",
        filteredDS->name,
        std::accumulate(node->byVariables.begin(), node->byVariables.end(), std::string(),
            [](const std::string& a, const std::string& b) -> std::string {
                return a.empty() ? b : a + ", " + b;
            }));

    // Handle NODUPKEY option
    DataSet* sortedDS = filteredDS;
    if (node->nodupkey) {
        std::string tempDSName = "TEMP_SORT_NODUPKEY";
        auto tempDS = env.getOrCreateDataset(tempDSName, tempDSName);
        tempDS->rows.clear();

        std::unordered_set<std::string> seenKeys;
        for (const auto& row : sortedDS->rows) {
            std::string key = "";
            for (const auto& var : node->byVariables) {
                auto it = row.columns.find(var);
                if (it != row.columns.end()) {
                    if (std::holds_alternative<double>(it->second)) {
                        key += std::to_string(std::get<double>(it->second)) + "_";
                    }
                    else if (std::holds_alternative<std::string>(it->second)) {
                        key += std::get<std::string>(it->second) + "_";
                    }
                    // Handle other data types as needed
                }
                else {
                    key += "NA_";
                }
            }

            if (seenKeys.find(key) == seenKeys.end()) {
                tempDS->rows.push_back(row);
                seenKeys.insert(key);
            }
            else {
                logLogger.info("Duplicate key '{}' found. Skipping duplicate observation.", key);
            }
        }

        sortedDS = tempDS.get();
        logLogger.info("Applied NODUPKEY option. {} observations remain after removing duplicates.", sortedDS->rows.size());
    }

    // Handle DUPLICATES option
    if (node->duplicates) {
        std::unordered_set<std::string> seenKeys;
        for (const auto& row : sortedDS->rows) {
            std::string key = "";
            for (const auto& var : node->byVariables) {
                auto it = row.columns.find(var);
                if (it != row.columns.end()) {
                    if (std::holds_alternative<double>(it->second)) {
                        key += std::to_string(std::get<double>(it->second)) + "_";
                    }
                    else if (std::holds_alternative<std::string>(it->second)) {
                        key += std::get<std::string>(it->second) + "_";
                    }
                    // Handle other data types as needed
                }
                else {
                    key += "NA_";
                }
            }

            if (seenKeys.find(key) != seenKeys.end()) {
                logLogger.info("Duplicate key '{}' found.", key);
            }
            else {
                seenKeys.insert(key);
            }
        }
    }

    // Determine the output dataset
    std::string outputDSName = node->outputDataSet.empty() ? node->inputDataSet : node->outputDataSet;
    DataSet* outputDS = env.getOrCreateDataset(outputDSName, outputDSName).get();
    if (outputDSName != sortedDS->name) {
        // Copy sortedDS to outputDS
        outputDS->rows = sortedDS->rows;
        logLogger.info("Sorted data copied to output dataset '{}'.", outputDSName);
    }
    else {
        // Overwrite the input dataset
        inputDS->rows = sortedDS->rows;
        logLogger.info("Input dataset '{}' overwritten with sorted data.", inputDS->name);
    }

    logLogger.info("PROC SORT executed successfully. Output dataset '{}' has {} observations.",
        outputDSName, outputDS->rows.size());
}

void Interpreter::executeProcMeans(ProcMeansNode* node) {
    logLogger.info("Executing PROC MEANS on dataset '{}'.", node->datasetName);
    try {
        auto dataset = env.getOrCreateDataset("", node->datasetName);
        lstLogger.info("PROC MEANS Results for Dataset '{}':", dataset->name);
        if (!env.title.empty()) {
            lstLogger.info("Title: {}", env.title);
        }

        // Calculate means for specified variables
        std::unordered_map<std::string, double> sums;
        std::unordered_map<std::string, int> counts;

        for (const auto& var : node->varNames) {
            sums[var] = 0.0;
            counts[var] = 0;
        }

        for (const auto& row : dataset->rows) {
            for (const auto& var : node->varNames) {
                auto it = row.columns.find(var);
                if (it != row.columns.end() && std::holds_alternative<double>(it->second)) {
                    sums[var] += std::get<double>(it->second);
                    counts[var]++;
                }
            }
        }

        // Print the means
        lstLogger.info("Variable\tMean");
        for (const auto& var : node->varNames) {
            if (counts[var] > 0) {
                double mean = sums[var] / counts[var];
                lstLogger.info("{}\t{:.2f}", var, mean);
            }
            else {
                lstLogger.info("{}\t.", var); // Missing values
            }
        }
    }
    catch (const std::runtime_error& e) {
        logLogger.error("PROC MEANS failed: {}", e.what());
    }
}

void Interpreter::executeIfElse(IfElseIfNode* node) {
    // Evaluate primary IF condition
    Value cond = evaluate(node->condition.get());
    double d = toNumber(cond);
    logLogger.info("Evaluating IF condition: {}", d);

    if (d != 0.0) { // Non-zero is true
        logLogger.info("Condition is TRUE. Executing THEN statements.");
        for (const auto& stmt : node->thenStatements) {
            if (auto block = dynamic_cast<BlockNode*>(stmt.get())) {
                executeBlock(block);
            }
            else {
                execute(stmt.get());
            }
        }
        return;
    }

    // Iterate through ELSE IF branches
    for (const auto& branch : node->elseIfBranches) {
        Value elseIfCond = evaluate(branch.first.get());
        double elseIfD = toNumber(elseIfCond);
        logLogger.info("Evaluating ELSE IF condition: {}", elseIfD);

        if (elseIfD != 0.0) { // Non-zero is true
            logLogger.info("ELSE IF condition is TRUE. Executing ELSE IF statements.");
            for (const auto& stmt : branch.second) {
                if (auto block = dynamic_cast<BlockNode*>(stmt.get())) {
                    executeBlock(block);
                }
                else {
                    execute(stmt.get());
                }
            }
            return;
        }
    }

    // Execute ELSE statements if no conditions were true
    if (!node->elseStatements.empty()) {
        logLogger.info("All conditions FALSE. Executing ELSE statements.");
        for (const auto& stmt : node->elseStatements) {
            if (auto block = dynamic_cast<BlockNode*>(stmt.get())) {
                executeBlock(block);
            }
            else {
                execute(stmt.get());
            }
        }
    }
    else {
        logLogger.info("All conditions FALSE. No ELSE statements to execute.");
    }
}


void Interpreter::executeBlock(BlockNode* node) {
    for (const auto& stmt : node->statements) {
        execute(stmt.get());
    }
}

Value Interpreter::evaluateFunctionCall(FunctionCallNode* node) {
    std::string func = node->functionName;
    // Convert function name to lowercase for case-insensitive matching
    std::transform(func.begin(), func.end(), func.begin(), ::tolower);

    if (func == "substr") {
        // substr(string, position, length)
        if (node->arguments.size() < 2 || node->arguments.size() > 3) {
            throw std::runtime_error("substr function expects 2 or 3 arguments.");
        }
        std::string str = std::get<std::string>(evaluate(node->arguments[0].get()));
        double pos = toNumber(evaluate(node->arguments[1].get()));
        int position = static_cast<int>(pos) - 1; // SAS substr is 1-based
        int length = (node->arguments.size() == 3) ? static_cast<int>(toNumber(evaluate(node->arguments[2].get()))) : str.length() - position;
        if (position < 0 || position >= static_cast<int>(str.length())) {
            return std::string(""); // Out of bounds
        }
        if (position + length > static_cast<int>(str.length())) {
            length = str.length() - position;
        }
        return str.substr(position, length);
    }
    else if (func == "trim") {
        // trim(string)
        if (node->arguments.size() != 1) {
            throw std::runtime_error("trim function expects 1 argument.");
        }
        std::string str = std::get<std::string>(evaluate(node->arguments[0].get()));
        // Remove trailing whitespace
        size_t endpos = str.find_last_not_of(" \t\r\n");
        if (std::string::npos != endpos) {
            str = str.substr(0, endpos + 1);
        }
        else {
            str.clear(); // All spaces
        }
        return str;
    }
    else if (func == "left") {
        // left(string)
        if (node->arguments.size() != 1) {
            throw std::runtime_error("left function expects 1 argument.");
        }
        std::string str = std::get<std::string>(evaluate(node->arguments[0].get()));
        // Remove leading whitespace
        size_t startpos = str.find_first_not_of(" \t\r\n");
        if (std::string::npos != startpos) {
            str = str.substr(startpos);
        }
        else {
            str.clear(); // All spaces
        }
        return str;
    }
    else if (func == "right") {
        // right(string)
        if (node->arguments.size() != 1) {
            throw std::runtime_error("right function expects 1 argument.");
        }
        std::string str = std::get<std::string>(evaluate(node->arguments[0].get()));
        // Remove trailing whitespace
        size_t endpos = str.find_last_not_of(" \t\r\n");
        if (std::string::npos != endpos) {
            str = str.substr(0, endpos + 1);
        }
        else {
            str.clear(); // All spaces
        }
        return str;
    }
    else if (func == "upcase") {
        // upcase(string)
        if (node->arguments.size() != 1) {
            throw std::runtime_error("upcase function expects 1 argument.");
        }
        std::string str = std::get<std::string>(evaluate(node->arguments[0].get()));
        std::transform(str.begin(), str.end(), str.begin(), ::toupper);
        return str;
    }
    else if (func == "lowcase") {
        // lowcase(string)
        if (node->arguments.size() != 1) {
            throw std::runtime_error("lowcase function expects 1 argument.");
        }
        std::string str = std::get<std::string>(evaluate(node->arguments[0].get()));
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        return str;
    }
    else if (func == "sqrt") {
        Value argVal = evaluate(node->arguments[0].get());
        double argNum = toNumber(argVal);

        if (argNum < 0) {
            logLogger.warn("sqrt() received a negative value. Returning NaN.");
            return std::nan("");
        }
        return std::sqrt(argNum);
    }
    else if (func == "abs") {
        // abs(number)
        if (node->arguments.size() != 1) {
            throw std::runtime_error("abs function expects 1 argument.");
        }
        double num = toNumber(evaluate(node->arguments[0].get()));
        return std::abs(num);
    }
    else if (func == "log") {
        Value argVal = evaluate(node->arguments[0].get());
        double argNum = toNumber(argVal);

        if (argNum <= 0) {
            logLogger.warn("log() received a non-positive value. Returning NaN.");
            return std::nan("");
        }
        return std::log(argNum);
    }
    else if (func == "ceil") {
        // ceil(number)
        if (node->arguments.size() != 1) {
            throw std::runtime_error("ceil function expects 1 argument.");
        }
        double num = toNumber(evaluate(node->arguments[0].get()));
        return std::ceil(num);
        }
    else if (func == "floor") {
            // floor(number)
            if (node->arguments.size() != 1) {
                throw std::runtime_error("floor function expects 1 argument.");
            }
            double num = toNumber(evaluate(node->arguments[0].get()));
            return std::floor(num);
            }
    else if (func == "round") {
                // round(number, decimal_places)
                if (node->arguments.size() < 1 || node->arguments.size() > 2) {
                    throw std::runtime_error("round function expects 1 or 2 arguments.");
                }
                double num = toNumber(evaluate(node->arguments[0].get()));
                int decimal = 0;
                if (node->arguments.size() == 2) {
                    decimal = static_cast<int>(toNumber(evaluate(node->arguments[1].get())));
                }
                double factor = std::pow(10.0, decimal);
                return std::round(num * factor) / factor;
                }
    else if (func == "exp") {
                    // exp(number)
                    if (node->arguments.size() != 1) {
                        throw std::runtime_error("exp function expects 1 argument.");
                    }
                    double num = toNumber(evaluate(node->arguments[0].get()));
                    return std::exp(num);
                    }
    else if (func == "log10") {
                        // log10(number)
                        if (node->arguments.size() != 1) {
                            throw std::runtime_error("log10 function expects 1 argument.");
                        }
                        double num = toNumber(evaluate(node->arguments[0].get()));
                        if (num <= 0.0) {
                            throw std::runtime_error("log10 function argument must be positive.");
                        }
                        return std::log10(num);
                        }
    // Date and Time Functions
    else if (func == "today") {
        // today()
        if (node->arguments.size() != 0) {
            throw std::runtime_error("today function expects no arguments.");
        }
        std::time_t t = std::time(nullptr);
        std::tm* tm_ptr = std::localtime(&t);
        // Return date as YYYYMMDD integer
        int year = tm_ptr->tm_year + 1900;
        int month = tm_ptr->tm_mon + 1;
        int day = tm_ptr->tm_mday;
        int date_int = year * 10000 + month * 100 + day;
        return static_cast<double>(date_int);
        }
    else if (func == "datepart") {
            // datepart(datetime)
            if (node->arguments.size() != 1) {
                throw std::runtime_error("datepart function expects 1 argument.");
            }
            double datetime = toNumber(evaluate(node->arguments[0].get()));
            // Assuming datetime is in SAS datetime format (seconds since 1960-01-01)
            // Convert to date as YYYYMMDD
            // Placeholder implementation
            // Implement actual conversion based on SAS datetime format
            // For simplicity, return the datetime as is
            return datetime;
            }
    else if (func == "timepart") {
                // timepart(datetime)
                if (node->arguments.size() != 1) {
                    throw std::runtime_error("timepart function expects 1 argument.");
                }
                double datetime = toNumber(evaluate(node->arguments[0].get()));
                // Assuming datetime is in SAS datetime format (seconds since 1960-01-01)
                // Convert to time as HHMMSS
                // Placeholder implementation
                // Implement actual conversion based on SAS datetime format
                // For simplicity, return the datetime as is
                return datetime;
                }
    else if (func == "intck") {
                    // intck(interval, start_date, end_date)
                    if (node->arguments.size() != 3) {
                        throw std::runtime_error("intck function expects 3 arguments.");
                    }
                    std::string interval = std::get<std::string>(evaluate(node->arguments[0].get()));
                    double start = toNumber(evaluate(node->arguments[1].get()));
                    double end = toNumber(evaluate(node->arguments[2].get()));

                    // Placeholder implementation for 'day' interval
                    if (interval == "day") {
                        int days = static_cast<int>(end - start);
                        return static_cast<double>(days);
                    }
                    else {
                        throw std::runtime_error("Unsupported interval in intck function: " + interval);
                    }
                    }
    else if (func == "intnx") {
		// intnx(interval, start_date, increment, alignment)
		if (node->arguments.size() < 3 || node->arguments.size() > 4) {
			throw std::runtime_error("intnx function expects 3 or 4 arguments.");
		}
		std::string interval = std::get<std::string>(evaluate(node->arguments[0].get()));
		double start = toNumber(evaluate(node->arguments[1].get()));
		double increment = toNumber(evaluate(node->arguments[2].get()));
		std::string alignment = "beginning"; // Default alignment
		if (node->arguments.size() == 4) {
			alignment = std::get<std::string>(evaluate(node->arguments[3].get()));
		}

		// Placeholder implementation for 'day' interval
		if (interval == "day") {
			double new_date = start + increment;
			return new_date;
		}
		else {
			throw std::runtime_error("Unsupported interval in intnx function: " + interval);
		}
	}
    else {
        throw std::runtime_error("Unsupported function: " + func);
    }
}

void Interpreter::executeMerge(MergeStatementNode* node) {
    logLogger.info("Executing MERGE statement with datasets:");
    for (const auto& ds : node->datasets) {
        logLogger.info(" - {}", ds);
    }

    // Ensure all datasets exist
    std::vector<DataSet*> mergeDatasets;
    for (const auto& dsName : node->datasets) {
        DataSet* ds = env.getOrCreateDataset(dsName, dsName).get();
        if (!ds) {
            throw std::runtime_error("Dataset not found for MERGE: " + dsName);
        }
        mergeDatasets.push_back(ds);
    }

    // Check if BY statement has been specified
    if (byVariables.empty()) {
        throw std::runtime_error("MERGE statement requires a preceding BY statement.");
    }

    // Sort all datasets by BY variables
    for (auto ds : mergeDatasets) {
        Sorter::sortDataset(ds, byVariables);
        logLogger.info("Dataset '{}' sorted by BY variables.", ds->name);
    }

    // Initialize iterators for each dataset
    std::vector<size_t> iterators(mergeDatasets.size(), 0);
    size_t numDatasets = mergeDatasets.size();

    // Create or clear the output dataset
    auto outputDataSet = env.getCurrentDataSet();
    outputDataSet->rows.clear();

    bool continueMerging = true;

    while (continueMerging) {
        // Collect current BY variable values from each dataset
        std::vector<std::vector<double>> currentBYValues(numDatasets, std::vector<double>());
        bool anyDatasetHasRows = false;

        for (size_t i = 0; i < numDatasets; ++i) {
            if (iterators[i] < mergeDatasets[i]->rows.size()) {
                anyDatasetHasRows = true;
                const Row& row = mergeDatasets[i]->rows[iterators[i]];
                std::vector<double> byVals;
                for (const auto& var : byVariables) {
                    double val = 0.0;
                    auto it = row.columns.find(var);
                    if (it != row.columns.end() && std::holds_alternative<double>(it->second)) {
                        val = std::get<double>(it->second);
                    }
                    byVals.push_back(val);
                }
                currentBYValues[i] = byVals;
            }
        }

        if (!anyDatasetHasRows) {
            break; // All datasets have been fully iterated
        }

        // Check for data type consistency across datasets for BY variables
        for (size_t j = 0; j < byVariables.size(); ++j) {
            double referenceVal = currentBYValues[0][j];
            for (size_t i = 1; i < numDatasets; ++i) {
                if (currentBYValues[i][j] != referenceVal) {
                    throw std::runtime_error("Data type mismatch for BY variable '" + byVariables[j] + "' across datasets.");
                }
            }
        }

        // Determine the minimum BY values across datasets
        std::vector<double> minBYValues = currentBYValues[0];
        for (size_t i = 1; i < numDatasets; ++i) {
            for (size_t j = 0; j < byVariables.size(); ++j) {
                if (currentBYValues[i][j] < minBYValues[j]) {
                    minBYValues[j] = currentBYValues[i][j];
                }
                else if (currentBYValues[i][j] > minBYValues[j]) {
                    // No change
                }
            }
        }

        // Collect all rows from datasets that match the min BY values
        std::vector<Row> matchedRows;
        for (size_t i = 0; i < numDatasets; ++i) {
            if (iterators[i] < mergeDatasets[i]->rows.size()) {
                bool match = true;
                for (size_t j = 0; j < byVariables.size(); ++j) {
                    if (currentBYValues[i][j] != minBYValues[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    matchedRows.push_back(mergeDatasets[i]->rows[iterators[i]]);
                    iterators[i]++; // Move iterator forward
                }
            }
        }

        bool allDatasetsHaveRows = true;
        for (size_t i = 0; i < numDatasets; ++i) {
            if (iterators[i] >= mergeDatasets[i]->rows.size()) {
                allDatasetsHaveRows = false;
                break;
            }
        }

        if (!allDatasetsHaveRows) {
            // Handle unmatched keys by including remaining rows from datasets
            for (size_t i = 0; i < numDatasets; ++i) {
                while (iterators[i] < mergeDatasets[i]->rows.size()) {
                    Row rowCopy = mergeDatasets[i]->rows[iterators[i]];
                    iterators[i]++;
                    // Merge with existing outputRow if necessary
                    outputDataSet->rows.push_back(rowCopy);
                }
            }
            break;
        }

        // Collect existing variable names to detect conflicts
        std::unordered_set<std::string> existingVars;
        for (const auto& var : byVariables) {
            existingVars.insert(var);
        }

        // Merge the matched rows into a single row
        Row mergedRow;
        for (const auto& row : matchedRows) {
            for (const auto& col : row.columns) {
                // Avoid overwriting BY variables
                if (std::find(byVariables.begin(), byVariables.end(), col.first) != byVariables.end()) {
                    mergedRow.columns[col.first] = col.second;
                }
                else {
                    // Handle variable name conflicts by prefixing with dataset name
                    if (mergedRow.columns.find(col.first) == mergedRow.columns.end()) {
                        mergedRow.columns[col.first] = col.second;
                    }
                    else {
                        std::string newColName = row.columns.begin()->first + "_" + col.first;
                        mergedRow.columns[newColName] = col.second;
                    }
                }
            }
        }

        // Append the merged row to the output dataset
        outputDataSet->rows.push_back(mergedRow);
    }

    logLogger.info("MERGE statement executed successfully. Output dataset '{}' has {} observations.",
        outputDataSet->name, outputDataSet->rows.size());
}


void Interpreter::executeBy(ByStatementNode* node) {
    logLogger.info("Executing BY statement with variables:");
    for (const auto& var : node->variables) {
        logLogger.info(" - {}", var);
    }

    // Store the BY variables in the interpreter's context
    byVariables = node->variables;

    // Ensure that the BY variables are present in all datasets to be merged
    // This can be implemented as needed
}

void Interpreter::executeDoLoop(DoLoopNode* node) {
    logLogger.info("Entering DO loop");

    // Push the loop context onto the stack
    loopStack.emplace(std::make_pair(node, 0));

    // Set a maximum number of iterations to prevent infinite loops
    const size_t MAX_ITERATIONS = 1000;

    while (!loopStack.empty()) {
        DoLoopNode* currentLoop = loopStack.top().first;
        size_t& iterationCount = loopStack.top().second;

        if (iterationCount >= MAX_ITERATIONS) {
            logLogger.error("Potential infinite loop detected in DO loop. Exiting loop.");
            loopStack.pop();
            break;
        }

        bool conditionMet = true;

        if (currentLoop->condition) {
            Value condValue = evaluate(currentLoop->condition.get());
            if (currentLoop->isWhile) {
                conditionMet = std::holds_alternative<double>(condValue) ? std::get<double>(condValue) != 0.0 : false;
            }
            else { // DO UNTIL
                conditionMet = !(std::holds_alternative<double>(condValue) ? std::get<double>(condValue) != 0.0 : false);
            }
        }

        if (conditionMet) {
            // Execute the loop body
            execute(currentLoop->body.get());
        }
        else {
            // Exit the loop
            loopStack.pop();
            logLogger.info("Exiting DO loop");
            break;
        }

        // In this simplified implementation, we assume the loop condition is re-evaluated after each iteration
    }
}

void Interpreter::executeEnd(EndNode* node) {
    if (loopStack.empty()) {
        throw std::runtime_error("END statement encountered without a corresponding DO loop.");
    }

    // Pop the current loop context to signify exiting the loop
    loopStack.pop();
    logLogger.info("Exiting DO loop via END statement");
}

