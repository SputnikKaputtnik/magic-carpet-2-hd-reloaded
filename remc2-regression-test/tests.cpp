// Unit-tests.cpp : This file contains the main function. This is where the program execution starts and ends.
//
#include <chrono>
#include <thread>
#include <iostream>
#include <string>
#include <cstdlib>
#include "regression-tests.h"


// Selects which tests to run.  Without arguments everything runs, as before.
struct TestSelection
{
	bool runLevels = true;
	bool runAfterload = true;
	int singleLevel = 0;   // 0 = every level
};

static void PrintUsage()
{
	std::cout
		<< "remc2-regression-test [options]\n"
		<< "  --levels          only the level regression tests\n"
		<< "  --afterload       only the afterload regression tests\n"
		<< "  --level <n>       only level n (implies --levels)\n"
		<< "  --help            this text\n"
		<< "Without options every test runs.  Exit code is the number of failures.\n";
}

int CountFailedRegressionTests(const TestSelection& selection) {
	int numFailedTests = 0;
	//run_regtest(level,testType,indexOfRegression,indexOfSavePosition(-1 - no load),isRecorded)
	enum TestType {
		BeginLevelNoActions = 0,
		AfterloadNoActions = 1,
		BeginLevelWithActions = 2,
		AfterloadWithActions = 3
	};
	if (selection.runLevels)
	{
		Logger->info("\n--- Level regressions tests ---");
		for (int i = 1; i <= 25; i++)
			if (i != 22 && i != 25)
				if (selection.singleLevel == 0 || selection.singleLevel == i)
					if (run_regtest(i) != 0)
					{
						numFailedTests++;
					}
	}

	if (selection.runAfterload)
	{
		Logger->info("--- Afterload regressions tests ---");

		if (run_regtest(2, TestType::AfterloadNoActions, 1, 2) != 0) numFailedTests++;
		if (run_regtest(2, TestType::BeginLevelWithActions, 2, 1, "Levels-1-5-Recording.bin", 25) != 0) numFailedTests++;
	}
	//if (run_regtest(1, TestType::BeginLevelWithActions, 3, -1, "Levels-1-5-Recording.bin", 3000) != 0) numFailedTests++;
	//if (run_regtest(1, true, 2, -1, "c:/prenos/remc2-dev2/remc2/x64/Debug/memimages/regressions/afterloadtest2/Levels-1-5-Recording.bin",25) != 0) numFailedTests++;


	// diff in level 22:
	//   the first frame with a diff has it at 0x7dba and the following bytes:
	//      buffer[32186] = 53   adress[32186] = 150
	//      buffer[32187] = 120   adress[32187] = 189
	//      buffer[32194] = 18   adress[32194] = 0
	//      buffer[32195] = 4   adress[32195] = 0
	//   in type_entity_0x6E8E struct_0x6E8E[1000];
	//   size of struct_0x6E8E[1000] is 0x29040 = 168000
	//   168 byte per element
	//   -> diff in the 23rd element struct_0x6E8E[22] at position 140
	//       -> maxMana_0x8C_140 and playerEntityIndex_0x94_148

	// diff in level 25
	// byte_counter_current_objective_box_0x36E04 = 0 instead of 200

	return numFailedTests;
}

int main(int argc, char** argv)
{
	//if (CommandLineParams.DoShowDebugMessages1()) -- for suppress messages
	int numFailedTests = 0;

	TestSelection selection;
	for (int i = 1; i < argc; i++)
	{
		const std::string argument = argv[i];
		if (argument == "--levels")
		{
			selection.runAfterload = false;
		}
		else if (argument == "--afterload")
		{
			selection.runLevels = false;
		}
		else if (argument == "--level" && i + 1 < argc)
		{
			selection.singleLevel = std::atoi(argv[++i]);
			selection.runAfterload = false;
		}
		else if (argument == "--help" || argument == "-h")
		{
			PrintUsage();
			return 0;
		}
		else
		{
			std::cout << "Unknown option: " << argument << "\n";
			PrintUsage();
			return 1;
		}
	}

	InitializeLogging(spdlog::level::info);
	numFailedTests += CountFailedRegressionTests(selection);

	if (numFailedTests == 0)
	{
		Logger->info("All tests passed");
	}
	else
	{
		Logger->error("{} tests failed!", numFailedTests);
	}	

	std::this_thread::sleep_for(std::chrono::milliseconds(2000));
	return numFailedTests;
}
