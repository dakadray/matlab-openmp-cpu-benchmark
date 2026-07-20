function suite = runBenchmarkProfileSuite(varargin)
% runBenchmarkProfileSuite Run a set of benchmark profiles as one local suite.
%
% By default this runs smoke, quick, standard, and stress.

    parser = inputParser;
    parser.addParameter('Profiles', {'smoke', 'quick', 'standard', 'stress'});
    parser.addParameter('Threads', 'all');
    parser.addParameter('SaveTag', 'local_reference', @(x) ischar(x) || isstring(x));
    parser.addParameter('ForceBuild', false, @(x) islogical(x) || isnumeric(x));
    parser.parse(varargin{:});

    rootDir = fileparts(mfilename('fullpath'));
    addpath(rootDir);

    profiles = normalizeProfiles(parser.Results.Profiles);
    resultDir = fullfile(rootDir, 'results');
    if exist(resultDir, 'dir') ~= 7
        mkdir(resultDir);
    end

    baseTag = makeSuiteTag(parser.Results.SaveTag);
    diaryFile = fullfile(resultDir, [baseTag '_diary.txt']);
    diary(diaryFile);
    cleanup = onCleanup(@() diary('off'));

    fprintf('Local CPU benchmark suite: %s\n', baseTag);
    fprintf('Profiles: %s\n', strjoin(profiles, ', '));
    fprintf('Threads: %s\n', tagValue(parser.Results.Threads));
    fprintf('\n');

    suite = struct();
    suite.baseTag = baseTag;
    suite.createdAt = datetime('now');
    suite.rootDir = rootDir;
    suite.profiles = profiles;
    suite.threads = parser.Results.Threads;
    suite.diaryFile = diaryFile;
    suite.benchmarks = cell(numel(profiles), 1);
    suite.combinedSummary = table();

    for iProfile = 1:numel(profiles)
        profile = profiles{iProfile};
        fprintf('\n=== Running profile: %s (%d/%d) ===\n', ...
            profile, iProfile, numel(profiles));

        suite.benchmarks{iProfile} = benchmarkOpenMpCpu( ...
            'Profile', profile, ...
            'Threads', parser.Results.Threads, ...
            'ForceBuild', logical(parser.Results.ForceBuild) && iProfile == 1, ...
            'SaveTag', [baseTag '_' profile]);

        oneSummary = suite.benchmarks{iProfile}.summary;
        oneSummary.profile = repmat(string(profile), height(oneSummary), 1);
        oneSummary.suiteTag = repmat(string(baseTag), height(oneSummary), 1);
        if isempty(suite.combinedSummary)
            suite.combinedSummary = oneSummary;
        else
            suite.combinedSummary = [suite.combinedSummary; oneSummary]; %#ok<AGROW>
        end
    end

    suiteMatFile = fullfile(resultDir, [baseTag '_suite.mat']);
    suiteCsvFile = fullfile(resultDir, [baseTag '_suite_summary.csv']);
    save(suiteMatFile, 'suite');
    writetable(suite.combinedSummary, suiteCsvFile);

    fprintf('\nSuite finished.\n');
    fprintf('Saved:\n  %s\n  %s\n  %s\n', suiteMatFile, suiteCsvFile, diaryFile);

    clear cleanup;
end

function profiles = normalizeProfiles(value)
    if ischar(value) || isstring(value)
        profiles = cellstr(value);
    elseif iscell(value)
        profiles = value;
    else
        error('cpuBench:invalidProfiles', ...
            'Profiles must be a char, string, or cell array of profile names.');
    end

    profiles = cellfun(@(x) lower(strtrim(char(x))), profiles, 'UniformOutput', false);
    profiles = profiles(~cellfun(@isempty, profiles));
    profiles = unique(profiles, 'stable');

    validProfiles = {'smoke', 'quick', 'standard', 'stress'};
    invalidMask = ~ismember(profiles, validProfiles);
    if any(invalidMask)
        error('cpuBench:invalidProfiles', 'Unsupported profile(s): %s', ...
            strjoin(profiles(invalidMask), ', '));
    end
    if isempty(profiles)
        error('cpuBench:noProfiles', 'No profiles were requested.');
    end
end

function suiteTag = makeSuiteTag(saveTag)
    tag = regexprep(lower(char(saveTag)), '[^a-z0-9]+', '_');
    tag = regexprep(tag, '^_|_$', '');
    if isempty(tag)
        tag = 'local_reference';
    end
    suiteTag = [tag '_' char(datetime('now', 'Format', 'yyyyMMdd_HHmmss'))];
end

function out = tagValue(value)
    if isnumeric(value)
        out = sprintf('%g', value);
    else
        out = char(value);
    end
end
