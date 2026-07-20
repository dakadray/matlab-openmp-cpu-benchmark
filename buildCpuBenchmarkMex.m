function info = buildCpuBenchmarkMex(varargin)
% buildCpuBenchmarkMex Build the OpenMP MEX stiffness assembler.

    parser = inputParser;
    parser.addParameter('Force', false, @(x) islogical(x) || isnumeric(x));
    parser.parse(varargin{:});

    rootDir = fileparts(mfilename('fullpath'));
    srcFile = fullfile(rootDir, 'mex', 'stiffnessAssemblyMex.cpp');
    outFile = fullfile(rootDir, ['stiffnessAssemblyMex.' mexext]);

    info = struct();
    info.source = srcFile;
    info.output = outFile;
    info.success = false;
    info.openmp = false;
    info.message = '';
    info.attempts = {};
    info.createdAt = datetime('now');

    if exist(outFile, 'file') == 3 && ~parser.Results.Force
        info.success = true;
        info.message = ['Existing MEX found: ' outFile];
        return;
    end

    if exist(srcFile, 'file') ~= 2
        error('cpuBench:missingSource', 'Missing source file: %s', srcFile);
    end

    clear stiffnessAssemblyMex;
    attempts = mexBuildAttempts(rootDir, srcFile);
    lastError = [];
    for iAttempt = 1:numel(attempts)
        attempt = attempts{iAttempt};
        info.attempts{end + 1} = attempt.description; %#ok<AGROW>
        try
            mex(attempt.args{:});
            if exist(outFile, 'file') ~= 3 && exist(outFile, 'file') ~= 2
                error('cpuBench:mexMissing', 'mex finished but did not create %s.', outFile);
            end
            info.success = true;
            info.openmp = attempt.openmp;
            info.message = ['Built ' outFile ' using ' attempt.description];
            writeBuildInfo(rootDir, info);
            fprintf('%s\n', info.message);
            return;
        catch ME
            lastError = ME;
            info.message = ME.message;
        end
    end

    writeBuildInfo(rootDir, info);
    if isempty(lastError)
        error('cpuBench:mexBuildFailed', 'MEX build failed.');
    end
    rethrow(lastError);
end

function attempts = mexBuildAttempts(rootDir, srcFile)
    base = {'-outdir', rootDir, srcFile};
    attempts = {};

    cfg = mex.getCompilerConfigurations('C++', 'Selected');
    compilerText = '';
    if ~isempty(cfg)
        compilerText = lower(strjoin({cfg.Name, cfg.Manufacturer}, ' '));
    end

    if ispc
        if contains(compilerText, 'mingw') || contains(compilerText, 'gcc')
            attempts{end + 1} = struct( ...
                'description', 'MinGW/GCC OpenMP', ...
                'openmp', true, ...
                'args', {{'-outdir', rootDir, 'CXXFLAGS="$CXXFLAGS -fopenmp"', ...
                    'LDFLAGS="$LDFLAGS -fopenmp"', srcFile}});
        end
        attempts{end + 1} = struct( ...
            'description', 'MSVC OpenMP', ...
            'openmp', true, ...
            'args', {{'-outdir', rootDir, 'COMPFLAGS="$COMPFLAGS /openmp"', srcFile}});
    elseif ismac
        attempts{end + 1} = struct( ...
            'description', 'Clang OpenMP with libomp', ...
            'openmp', true, ...
            'args', {{'-outdir', rootDir, 'CXXFLAGS="$CXXFLAGS -Xpreprocessor -fopenmp"', ...
                'LDFLAGS="$LDFLAGS -lomp"', srcFile}});
    else
        attempts{end + 1} = struct( ...
            'description', 'GCC/Clang OpenMP', ...
            'openmp', true, ...
            'args', {{'-outdir', rootDir, 'CXXFLAGS="$CXXFLAGS -fopenmp"', ...
                'LDFLAGS="$LDFLAGS -fopenmp"', srcFile}});
    end

    attempts{end + 1} = struct( ...
        'description', 'serial fallback without OpenMP', ...
        'openmp', false, ...
        'args', {base});
end

function writeBuildInfo(rootDir, info)
    fid = fopen(fullfile(rootDir, 'mex_build_info.json'), 'w');
    if fid < 0
        return;
    end
    cleanup = onCleanup(@() fclose(fid));
    encoded = jsonencode(info, 'PrettyPrint', true);
    fprintf(fid, '%s\n', encoded);
end

