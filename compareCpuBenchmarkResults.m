function comparison = compareCpuBenchmarkResults(resultFiles)
% compareCpuBenchmarkResults Compare summary CSV files from multiple CPUs.

    if nargin < 1 || isempty(resultFiles)
        files = dir(fullfile(fileparts(mfilename('fullpath')), 'results', '*_summary.csv'));
        resultFiles = fullfile({files.folder}, {files.name});
    end

    if ischar(resultFiles) || isstring(resultFiles)
        resultFiles = cellstr(resultFiles);
    end
    if isempty(resultFiles)
        error('cpuBench:noResults', 'No summary CSV files found.');
    end

    tables = cell(numel(resultFiles), 1);
    for iFile = 1:numel(resultFiles)
        t = readtable(resultFiles{iFile});
        [~, name] = fileparts(resultFiles{iFile});
        t.resultFile = repmat(string(name), height(t), 1);
        tables{iFile} = t;
    end

    allRows = vertcat(tables{:});
    metric = allRows.medianStiffnessSeconds + allRows.medianSolveSeconds;
    allRows.medianAssemblySolveSeconds = metric;

    [groups, nx, ny, nz] = findgroups(allRows.nx, allRows.ny, allRows.nz);
    best = splitapply(@min, allRows.medianAssemblySolveSeconds, groups);
    allRows.speedupVsBestSameSize = best(groups) ./ allRows.medianAssemblySolveSeconds;

    comparison = sortrows(allRows, {'nx', 'ny', 'nz', 'medianAssemblySolveSeconds'});
    disp(comparison(:, {'resultFile', 'nx', 'ny', 'nz', 'dofs', ...
        'medianStiffnessSeconds', 'medianSolveSeconds', ...
        'medianAssemblySolveSeconds', 'speedupVsBestSameSize'}));
end

