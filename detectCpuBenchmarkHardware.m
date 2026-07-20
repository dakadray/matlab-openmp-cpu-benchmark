function hw = detectCpuBenchmarkHardware()
% detectCpuBenchmarkHardware Collect lightweight CPU and MATLAB metadata.

    hw = struct();
    hw.createdAt = datetime('now');
    hw.computer = computer;
    hw.matlabVersion = version;
    hw.maxNumCompThreads = safeMaxNumCompThreads();
    hw.logicalProcessors = detectLogicalProcessors();
    hw.physicalCores = NaN;
    hw.cpuName = '';
    hw.rawCpuInfo = '';

    if ispc
        hw = fillWindowsCpuInfo(hw);
    elseif isunix && ~ismac
        hw = fillLinuxCpuInfo(hw);
    elseif ismac
        hw = fillMacCpuInfo(hw);
    end

    if isempty(hw.cpuName)
        hw.cpuName = 'unknown CPU';
    end
end

function n = safeMaxNumCompThreads()
    try
        n = maxNumCompThreads;
    catch
        n = NaN;
    end
end

function n = detectLogicalProcessors()
    n = NaN;
    if ispc
        [status, out] = system('powershell -NoProfile -Command "(Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors"');
        if status == 0
            n = str2double(strtrim(out));
        end
    elseif ismac
        [status, out] = system('sysctl -n hw.logicalcpu');
        if status == 0
            n = str2double(strtrim(out));
        end
    elseif isunix
        [status, out] = system('nproc');
        if status == 0
            n = str2double(strtrim(out));
        end
    end

    if ~isfinite(n) || n < 1
        try
            n = java.lang.Runtime.getRuntime().availableProcessors();
        catch
            n = feature('numcores');
        end
    end
    n = max(1, round(double(n)));
end

function hw = fillWindowsCpuInfo(hw)
    [statusName, outName] = system('powershell -NoProfile -Command "(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)"');
    if statusName == 0
        hw.cpuName = strtrim(outName);
    end

    [statusCores, outCores] = system('powershell -NoProfile -Command "($p=Get-CimInstance Win32_Processor | Measure-Object -Property NumberOfCores -Sum).Sum"');
    if statusCores == 0
        hw.physicalCores = str2double(strtrim(outCores));
    end

    [statusRaw, outRaw] = system('powershell -NoProfile -Command "Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed | Format-List"');
    if statusRaw == 0
        hw.rawCpuInfo = strtrim(outRaw);
    end
end

function hw = fillLinuxCpuInfo(hw)
    [status, out] = system('lscpu');
    if status == 0
        hw.rawCpuInfo = strtrim(out);
        name = regexp(out, 'Model name:\s*(.+)', 'tokens', 'once');
        cores = regexp(out, 'Core\(s\) per socket:\s*(\d+)', 'tokens', 'once');
        sockets = regexp(out, 'Socket\(s\):\s*(\d+)', 'tokens', 'once');
        if ~isempty(name)
            hw.cpuName = strtrim(name{1});
        end
        if ~isempty(cores) && ~isempty(sockets)
            hw.physicalCores = str2double(cores{1}) * str2double(sockets{1});
        end
    end
end

function hw = fillMacCpuInfo(hw)
    [statusName, outName] = system('sysctl -n machdep.cpu.brand_string');
    if statusName == 0
        hw.cpuName = strtrim(outName);
    end
    [statusCore, outCore] = system('sysctl -n hw.physicalcpu');
    if statusCore == 0
        hw.physicalCores = str2double(strtrim(outCore));
    end
    [statusRaw, outRaw] = system('sysctl machdep.cpu hw.physicalcpu hw.logicalcpu');
    if statusRaw == 0
        hw.rawCpuInfo = strtrim(outRaw);
    end
end

