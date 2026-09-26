# Spec: Improve performace of surrounding wave height

## Overview
This specification describes how to improve the performance of the GetWaveDataAroundLocation function to only fetch the rows from the datatable once.

## Objective
Improve the performace of the GetWaveDataAroundLocation function.

## Requirements

### Functional Requirements
1. **FR-1**: The class WaveHeight shall have a function GetWaveDataAroundLocation.
2. **FR-2**: The function GetWaveDataAroundLocation shall only call waveSamplesData->FindRow once
2. **FR-2**: The function GetWaveDataAroundLocation shall only call waveSamplesMetaData->FindRow once

4.**FR-3**: Since the function waveHeightAndNormal is exposed to BP, don´t change it´s interface

### Non-Functional Requirements
1. **NFR-1**: The implementation shall be simple and readable
2. **NFR-2**: The code shall follow project coding standards

## Acceptance Criteria

## Implementation Details

### Suggested Approach
- Refactor the code, and let waveHeightAndNormal fetch the data Row and metadata row, and then pass it to a new function that does the calculation for waveHeightAndNormal 
- Refactor the code, and let GetWaveDataAroundLocation 
fetch the data Row and metadata row, and then pass it to a new function that does the calculation for waveHeightAndNormal 

## Notes for AI Agent
- Use c++
- Ensure proper UE logging categories are used

## Status
- [ ] Specification written
- [ ] Implementation complete
- [ ] Tests passing
- [ ] Code reviewed
- [ ] Merged to main

## Metadata
- **Created**: 2025-10-10
- **Author**: Development Team
- **Priority**: Low (Example)
- **Estimated Effort**: 15 minutes
