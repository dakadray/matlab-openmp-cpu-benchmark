function status = setCpuBenchmarkCpuMode(mode)
% setCpuBenchmarkCpuMode Configure process CPU affinity for server benchmarks.
%
% Supported modes:
%   pcores - Windows Intel hybrid CPUs: use highest EfficiencyClass CPUs.
%   pe     - Use all active CPUs, intended for Intel P+E runs.
%   amd    - Use all active CPUs, intended for AMD runs.
%   all    - Use all active CPUs.

    if nargin < 1 || isempty(mode)
        mode = 'all';
    end

    mode = lower(strtrim(char(mode)));

    switch mode
        case {'p', 'pcore', 'pcores'}
            mexMode = 'pcores';
        case {'pe', 'all', 'amd'}
            mexMode = mode;
        otherwise
            error('cpuBench:invalidCpuMode', ...
                'CPU mode must be ''pcores'', ''pe'', ''amd'', or ''all''.');
    end

    buildCpuBenchmarkMex();
    try
        status = stiffnessAssemblyMex('setCpuMode', mexMode);
    catch ME
        if strcmp(ME.identifier, 'cpuBench:unknownCommand') || ...
                contains(ME.message, 'Unknown command')
            buildCpuBenchmarkMex('Force', true);
            clear stiffnessAssemblyMex;
            status = stiffnessAssemblyMex('setCpuMode', mexMode);
        else
            rethrow(ME);
        end
    end
    if ~isfield(status, 'recommendedThreads') || isempty(status.recommendedThreads)
        status.recommendedThreads = max(1, feature('numcores'));
    end
    status.recommendedThreads = max(1, round(double(status.recommendedThreads)));

    setenv('OMP_NUM_THREADS', num2str(status.recommendedThreads));
    setenv('MKL_NUM_THREADS', num2str(status.recommendedThreads));
    setenv('OMP_PROC_BIND', 'spread');
    setenv('OMP_PLACES', 'threads');

    fprintf('CPU mode: %s\n', char(status.mode));
    fprintf('Recommended threads: %d\n', status.recommendedThreads);
    if isfield(status, 'selectedLogicalProcessors')
        fprintf('Selected logical processors: %s\n', ...
            mat2str(status.selectedLogicalProcessors));
    end
end
