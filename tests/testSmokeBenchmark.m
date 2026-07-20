function tests = testSmokeBenchmark
% testSmokeBenchmark Basic correctness test for the MEX benchmark path.

    tests = functiontests(localfunctions);
end

function testSmallRun(testCase)
    rootDir = fileparts(fileparts(mfilename('fullpath')));
    addpath(rootDir);
    bench = benchmarkOpenMpCpu('Profile', 'smoke', 'Repeats', 1, 'Threads', 1, ...
        'SaveTag', 'test');
    verifyGreaterThan(testCase, height(bench.detail), 0);
    verifyLessThan(testCase, max(bench.detail.relativeResidual), 1.0e-8);
    verifyTrue(testCase, all(bench.detail.stiffnessSeconds > 0));
    verifyTrue(testCase, all(bench.detail.solveSeconds > 0));
end

