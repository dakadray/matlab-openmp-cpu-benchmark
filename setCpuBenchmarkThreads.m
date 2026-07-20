function status = setCpuBenchmarkThreads(threadSpec)
% setCpuBenchmarkThreads Configure MATLAB and MEX threading for the benchmark.

    if nargin < 1 || isempty(threadSpec)
        threadSpec = 'all';
    end

    hw = detectCpuBenchmarkHardware();
    nThreads = resolveThreadSpec(threadSpec, hw.logicalProcessors);

    status = struct();
    status.requested = threadSpec;
    status.resolvedThreads = nThreads;
    status.hardware = hw;
    status.previousMatlabThreads = NaN;
    status.mexStatus = [];

    setenv('OMP_NUM_THREADS', num2str(nThreads));
    setenv('OMP_PROC_BIND', 'spread');
    setenv('OMP_PLACES', 'threads');
    setenv('MKL_NUM_THREADS', num2str(nThreads));

    try
        status.previousMatlabThreads = maxNumCompThreads(nThreads);
    catch
        status.previousMatlabThreads = NaN;
    end

    if exist('stiffnessAssemblyMex', 'file') == 3
        status.mexStatus = stiffnessAssemblyMex('setThreads', nThreads);
    end
end

function n = resolveThreadSpec(threadSpec, detectedLogical)
    if isnumeric(threadSpec)
        n = double(threadSpec);
    else
        spec = lower(strtrim(char(threadSpec)));
        switch spec
            case {'all', 'max', 'logical'}
                n = detectedLogical;
            case {'half'}
                n = max(1, floor(detectedLogical / 2));
            otherwise
                n = str2double(spec);
        end
    end

    if ~isfinite(n) || n < 1 || floor(n) ~= n
        error('cpuBench:invalidThreads', 'Thread spec must be ''all'' or a positive integer.');
    end
    n = round(n);
end

