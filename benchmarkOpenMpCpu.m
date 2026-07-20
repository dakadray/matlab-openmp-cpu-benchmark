function bench = benchmarkOpenMpCpu(varargin)
% benchmarkOpenMpCpu Run MATLAB solve + C++/OpenMP/MEX stiffness benchmark.

    parser = inputParser;
    parser.addParameter('Profile', 'standard', @(x) ischar(x) || isstring(x));
    parser.addParameter('Sizes', [], @(x) isempty(x) || (isnumeric(x) && size(x, 2) == 3));
    parser.addParameter('Repeats', [], @(x) isempty(x) || (isscalar(x) && x >= 1));
    parser.addParameter('Threads', 'all');
    parser.addParameter('ForceBuild', false, @(x) islogical(x) || isnumeric(x));
    parser.addParameter('SaveTag', '', @(x) ischar(x) || isstring(x));
    parser.parse(varargin{:});

    rootDir = fileparts(mfilename('fullpath'));
    addpath(rootDir);
    buildInfo = buildCpuBenchmarkMex('Force', logical(parser.Results.ForceBuild));
    threadStatus = setCpuBenchmarkThreads(parser.Results.Threads);

    sizes = parser.Results.Sizes;
    if isempty(sizes)
        sizes = defaultSizes(parser.Results.Profile);
    end

    repeats = parser.Results.Repeats;
    if isempty(repeats)
        repeats = defaultRepeats(parser.Results.Profile);
    end
    repeats = round(repeats);

    resultDir = fullfile(rootDir, 'results');
    if exist(resultDir, 'dir') ~= 7
        mkdir(resultDir);
    end

    hw = detectCpuBenchmarkHardware();
    runId = makeRunId(hw, parser.Results.SaveTag);
    fprintf('CPU benchmark run: %s\n', runId);
    fprintf('CPU: %s\n', hw.cpuName);
    fprintf('Threads requested: %s, resolved: %d\n', stringValue(parser.Results.Threads), ...
        threadStatus.resolvedThreads);
    fprintf('Sizes: %s, repeats: %d\n\n', mat2str(sizes), repeats);

    rows = struct([]);
    rowIndex = 0;

    for iSize = 1:size(sizes, 1)
        nx = sizes(iSize, 1);
        ny = sizes(iSize, 2);
        nz = sizes(iSize, 3);

        fprintf('Warmup %dx%dx%d ...\n', nx, ny, nz);
        [Kwarm, Fwarm] = stiffnessAssemblyMex(nx, ny, nz); %#ok<ASGLU>
        fixedWarm = fixedDofsForGrid(nx, ny, nz);
        freeWarm = true(size(Fwarm));
        freeWarm(fixedWarm) = false;
        uwarm = Kwarm(freeWarm, freeWarm) \ Fwarm(freeWarm); %#ok<NASGU>
        clear Kwarm Fwarm uwarm;

        for repeat = 1:repeats
            fprintf('Measure %dx%dx%d repeat %d/%d ... ', nx, ny, nz, repeat, repeats);

            tMexCall = tic;
            [K, F, mexProfile] = stiffnessAssemblyMex(nx, ny, nz);
            matlabMexCallSeconds = toc(tMexCall);

            tConstraint = tic;
            fixed = fixedDofsForGrid(nx, ny, nz);
            free = true(size(F));
            free(fixed) = false;
            Kff = K(free, free);
            Ff = F(free);
            constraintSeconds = toc(tConstraint);

            tSolve = tic;
            Ufree = Kff \ Ff;
            solveSeconds = toc(tSolve);

            tResidual = tic;
            relativeResidual = norm(Kff * Ufree - Ff) / max(norm(Ff), eps);
            residualSeconds = toc(tResidual);

            rowIndex = rowIndex + 1;
            rows(rowIndex).runId = runId; %#ok<AGROW>
            rows(rowIndex).cpuName = hw.cpuName;
            rows(rowIndex).logicalProcessors = hw.logicalProcessors;
            rows(rowIndex).physicalCores = hw.physicalCores;
            rows(rowIndex).requestedThreads = stringValue(parser.Results.Threads);
            rows(rowIndex).resolvedThreads = threadStatus.resolvedThreads;
            rows(rowIndex).actualMexThreads = mexProfile.actualThreads;
            rows(rowIndex).openmp = logical(mexProfile.openmp);
            rows(rowIndex).nx = nx;
            rows(rowIndex).ny = ny;
            rows(rowIndex).nz = nz;
            rows(rowIndex).elements = mexProfile.elements;
            rows(rowIndex).dofs = mexProfile.dofs;
            rows(rowIndex).freeDofs = nnz(free);
            rows(rowIndex).nnzK = nnz(K);
            rows(rowIndex).nnzKff = nnz(Kff);
            rows(rowIndex).repeat = repeat;
            rows(rowIndex).mexCallSeconds = matlabMexCallSeconds;
            rows(rowIndex).stiffnessSeconds = mexProfile.stiffnessSeconds;
            rows(rowIndex).elementSeconds = mexProfile.elementSeconds;
            rows(rowIndex).sortSeconds = mexProfile.sortSeconds;
            rows(rowIndex).compressSeconds = mexProfile.compressSeconds;
            rows(rowIndex).sparseFillSeconds = mexProfile.sparseFillSeconds;
            rows(rowIndex).constraintSeconds = constraintSeconds;
            rows(rowIndex).solveSeconds = solveSeconds;
            rows(rowIndex).residualSeconds = residualSeconds;
            rows(rowIndex).relativeResidual = relativeResidual;
            rows(rowIndex).rawTriplets = mexProfile.rawTriplets;
            rows(rowIndex).uniqueNonzeros = mexProfile.uniqueNonzeros;
            fprintf('K %.3fs, solve %.3fs, residual %.2e\n', ...
                rows(rowIndex).stiffnessSeconds, solveSeconds, relativeResidual);

            clear K F Kff Ff Ufree;
        end
    end

    detail = struct2table(rows);
    summary = summarizeRows(detail);

    bench = struct();
    bench.runId = runId;
    bench.createdAt = datetime('now');
    bench.rootDir = rootDir;
    bench.hardware = hw;
    bench.buildInfo = buildInfo;
    bench.threadStatus = threadStatus;
    bench.sizes = sizes;
    bench.repeats = repeats;
    bench.detail = detail;
    bench.summary = summary;

    csvDetail = fullfile(resultDir, [runId '_detail.csv']);
    csvSummary = fullfile(resultDir, [runId '_summary.csv']);
    matFile = fullfile(resultDir, [runId '.mat']);
    writetable(detail, csvDetail);
    writetable(summary, csvSummary);
    save(matFile, 'bench');

    fprintf('\nSummary by median time:\n');
    disp(summary(:, {'nx', 'ny', 'nz', 'dofs', 'resolvedThreads', ...
        'actualMexThreads', 'medianStiffnessSeconds', 'medianSolveSeconds', ...
        'medianTotalSeconds', 'medianRelativeResidual'}));
    fprintf('Saved:\n  %s\n  %s\n  %s\n', csvDetail, csvSummary, matFile);
end

function sizes = defaultSizes(profile)
    switch lower(strtrim(char(profile)))
        case 'smoke'
            sizes = [4 4 4; 6 6 6];
        case 'quick'
            sizes = [6 6 6; 8 8 8; 10 10 10];
        case 'standard'
            sizes = [8 8 8; 12 12 12; 16 16 16; 20 20 20; ...
                25 25 25; 30 30 30; 35 35 35; 40 40 40];
        case 'stress'
            sizes = [20 20 20; 25 25 25; 30 30 30; 35 35 35; 40 40 40];
        otherwise
            error('cpuBench:unknownProfile', 'Unknown profile: %s', char(profile));
    end
end

function repeats = defaultRepeats(profile)
    switch lower(strtrim(char(profile)))
        case 'smoke'
            repeats = 1;
        case 'quick'
            repeats = 2;
        case 'standard'
            repeats = 3;
        case 'stress'
            repeats = 2;
        otherwise
            repeats = 3;
    end
end

function fixed = fixedDofsForGrid(nx, ny, nz)
    fixed = zeros(3 * (ny + 1) * (nz + 1), 1);
    cursor = 0;
    for k = 0:nz
        for j = 0:ny
            node = 1 + 0 + (nx + 1) * (j + (ny + 1) * k);
            fixed(cursor + (1:3)) = 3 * node + (-2:0);
            cursor = cursor + 3;
        end
    end
end

function summary = summarizeRows(detail)
    [groups, nx, ny, nz] = findgroups(detail.nx, detail.ny, detail.nz);
    summary = table();
    summary.nx = nx;
    summary.ny = ny;
    summary.nz = nz;
    summary.elements = splitapply(@median, detail.elements, groups);
    summary.dofs = splitapply(@median, detail.dofs, groups);
    summary.freeDofs = splitapply(@median, detail.freeDofs, groups);
    summary.nnzK = splitapply(@median, detail.nnzK, groups);
    summary.resolvedThreads = splitapply(@median, detail.resolvedThreads, groups);
    summary.actualMexThreads = splitapply(@median, detail.actualMexThreads, groups);
    summary.medianStiffnessSeconds = splitapply(@median, detail.stiffnessSeconds, groups);
    summary.medianElementSeconds = splitapply(@median, detail.elementSeconds, groups);
    summary.medianSortSeconds = splitapply(@median, detail.sortSeconds, groups);
    summary.medianSolveSeconds = splitapply(@median, detail.solveSeconds, groups);
    summary.medianConstraintSeconds = splitapply(@median, detail.constraintSeconds, groups);
    summary.medianTotalSeconds = splitapply(@median, ...
        detail.stiffnessSeconds + detail.constraintSeconds + detail.solveSeconds, groups);
    summary.medianRelativeResidual = splitapply(@median, detail.relativeResidual, groups);
end

function runId = makeRunId(hw, tag)
    stamp = char(datetime('now', 'Format', 'yyyyMMdd_HHmmss'));
    cpu = regexprep(lower(hw.cpuName), '[^a-z0-9]+', '_');
    cpu = regexprep(cpu, '^_|_$', '');
    if strlength(string(cpu)) > 42
        cpu = extractBefore(string(cpu), 43);
        cpu = char(cpu);
    end
    if isempty(cpu)
        cpu = 'cpu';
    end
    tag = char(tag);
    if ~isempty(tag)
        tag = ['_' regexprep(lower(tag), '[^a-z0-9]+', '_')];
    end
    runId = [stamp '_' cpu tag];
end

function out = stringValue(value)
    if isnumeric(value)
        out = sprintf('%g', value);
    else
        out = char(value);
    end
end
