// Includes
#include <PetscSolver1DHandler.h>
#include <HDF5Utils.h>
#include <MathUtils.h>
#include <Constants.h>
#include <SuperCluster.h>

namespace xolotlSolver {

void PetscSolver1DHandler::createSolverContext(DM &da, int nx, double hx, int ny,
		double hy, int nz, double hz) {
	PetscErrorCode ierr;

	// Set the last temperature to 0
	lastTemperature = 0.0;

	// Reinitialize the connectivities in the network after updating the temperature
	// Get the temperature from the temperature handler
	auto temperature = temperatureHandler->getTemperature({0.0, 0.0, 0.0}, 0.0);

	// Update the network if the temperature changed
	if (!xolotlCore::equal(temperature, lastTemperature)) {
		network->setTemperature(temperature);
		lastTemperature = temperature;
	}

	// Recompute Ids and network size and redefine the connectivities
	network->reinitializeConnectivities();

	// Degrees of freedom is the total number of clusters in the network
	const int dof = network->size() + 2 * network->getAll("Super").size();

	// Initialize the all reactants pointer
	allReactants = network->getAll();

	/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	 Create distributed array (DMDA) to manage parallel grid and vectors
	 - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
	ierr = DMDACreate1d(PETSC_COMM_WORLD, DM_BOUNDARY_GHOSTED, nx, dof, 1,
	NULL, &da);
	checkPetscError(ierr, "PetscSolver1DHandler::createSolverContext: "
			"DMDACreate1d failed.");

	// Set the position of the surface
	surfacePosition = 0;
	if (movingSurface) surfacePosition = (int) (nx * portion / 100.0);

	// Generate the grid in the x direction
	generateGrid(nx, hx, surfacePosition);

	// Initialize the surface of the first advection handler corresponding to the
	// advection toward the surface (or a dummy one if it is deactivated)
	advectionHandlers[0]->setLocation(grid[surfacePosition]);

//	for (int i = 0; i < grid.size(); i++) {
//		std::cout << grid[i] - grid[surfacePosition] << " ";
//	}
//	std::cout << std::endl;

	// Set the size of the partial derivatives vectors
	clusterPartials.resize(dof, 0.0);
	reactingPartialsForCluster.resize(dof, 0.0);

	/*  The only spatial coupling in the Jacobian is due to diffusion.
	 *  The ofill (thought of as a dof by dof 2d (row-oriented) array represents
	 *  the nonzero coupling between degrees of freedom at one point with degrees
	 *  of freedom on the adjacent point to the left or right. A 1 at i,j in the
	 *  ofill array indicates that the degree of freedom i at a point is coupled
	 *  to degree of freedom j at the adjacent point.
	 *  In this case ofill has only a few diagonal entries since the only spatial
	 *  coupling is regular diffusion.
	 */
	PetscInt *ofill, *dfill;
	ierr = PetscMalloc(dof * dof * sizeof(PetscInt), &ofill);
	checkPetscError(ierr, "PetscSolver1DHandler::createSolverContext: "
			"PetscMalloc (ofill) failed.");
	ierr = PetscMalloc(dof * dof * sizeof(PetscInt), &dfill);
	checkPetscError(ierr, "PetscSolver1DHandler::createSolverContext: "
			"PetscMalloc (dfill) failed.");
	ierr = PetscMemzero(ofill, dof * dof * sizeof(PetscInt));
	checkPetscError(ierr, "PetscSolver1DHandler::createSolverContext: "
			"PetscMemzero (ofill) failed.");
	ierr = PetscMemzero(dfill, dof * dof * sizeof(PetscInt));
	checkPetscError(ierr, "PetscSolver1DHandler::createSolverContext: "
			"PetscMemzero (dfill) failed.");

	// Fill ofill, the matrix of "off-diagonal" elements that represents diffusion
	diffusionHandler->initializeOFill(network, ofill);
	// Loop on the advection handlers to account the other "off-diagonal" elements
	for (int i = 0; i < advectionHandlers.size(); i++) {
		advectionHandlers[i]->initialize(network, ofill);
	}

	// Initialize the modified trap-mutation handler and the bubble bursting one here
	// because they add connectivity
	mutationHandler->initialize(surfacePosition, network, grid);
	burstingHandler->initialize(surfacePosition, network, grid);

	// Get the diagonal fill
	getDiagonalFill(dfill, dof * dof);

	// Load up the block fills
	ierr = DMDASetBlockFills(da, dfill, ofill);
	checkPetscError(ierr, "PetscSolver1DHandler::createSolverContext: "
			"DMDASetBlockFills failed.");

	// Free the temporary fill arrays
	ierr = PetscFree(ofill);
	checkPetscError(ierr, "PetscSolver1DHandler::createSolverContext: "
			"PetscFree (ofill) failed.");
	ierr = PetscFree(dfill);
	checkPetscError(ierr, "PetscSolver1DHandler::createSolverContext: "
			"PetscFree (dfill) failed.");

	return;
}

void PetscSolver1DHandler::initializeConcentration(DM &da, Vec &C) {
	PetscErrorCode ierr;

	// Pointer for the concentration vector
	PetscScalar **concentrations;
	ierr = DMDAVecGetArrayDOF(da, C, &concentrations);
	checkPetscError(ierr, "PetscSolver1DHandler::initializeConcentration: "
			"DMDAVecGetArrayDOF failed.");

	// Get the local boundaries
	PetscInt xs, xm;
	ierr = DMDAGetCorners(da, &xs, NULL, NULL, &xm, NULL, NULL);
	checkPetscError(ierr, "PetscSolver1DHandler::initializeConcentration: "
			"DMDAGetCorners failed.");

	// Get the last time step written in the HDF5 file
	int tempTimeStep = -2;
	bool hasConcentrations = xolotlCore::HDF5Utils::hasConcentrationGroup(networkName,
			tempTimeStep);

	// Get the actual surface position if concentrations were stored
	if (hasConcentrations)
		surfacePosition = xolotlCore::HDF5Utils::readSurface1D(networkName, tempTimeStep);

	// Get the total size of the grid for the boundary conditions
	int xSize = grid.size();

	// Initialize the flux handler
	fluxHandler->initializeFluxHandler(network, surfacePosition, grid);

	// Initialize the grid for the diffusion
	diffusionHandler->initializeDiffusionGrid(advectionHandlers, grid);

	// Initialize the grid for the advection
	advectionHandlers[0]->initializeAdvectionGrid(advectionHandlers, grid);

	// Pointer for the concentration vector at a specific grid point
	PetscScalar *concOffset;

	// Get all the super clusters
	auto superClusters = network->getAll("Super");
	// Degrees of freedom is the total number of clusters in the network
	const int dof = network->size() + 2 * superClusters.size();

	// Get the single vacancy ID
	auto singleVacancyCluster = network->get(xolotlCore::vType, 1);
	int vacancyIndex = -1;
	if (singleVacancyCluster)
		vacancyIndex = singleVacancyCluster->getId() - 1;

	// Loop on all the grid points
	for (int i = xs; i < xs + xm; i++) {
		concOffset = concentrations[i];

		// Loop on all the clusters to initialize at 0.0
		for (int n = 0; n < dof; n++) {
			concOffset[n] = 0.0;
		}

		// Initialize the vacancy concentration
		if (i > surfacePosition && i < xSize - 1 && singleVacancyCluster) {
			concOffset[vacancyIndex] = initialVConc;
		}
	}

	// If the concentration must be set from the HDF5 file
	if (hasConcentrations) {
		// Loop on the full grid
		for (int i = 0; i < xSize; i++) {
			// Read the concentrations from the HDF5 file
			auto concVector = xolotlCore::HDF5Utils::readGridPoint(networkName,
					tempTimeStep, i);

			// Change the concentration only if we are on the locally owned part of the grid
			if (i >= xs && i < xs + xm) {
				concOffset = concentrations[i];
				// Loop on the concVector size
				for (int l = 0; l < concVector.size(); l++) {
					concOffset[(int) concVector.at(l).at(0)] =
							concVector.at(l).at(1);
				}
			}
		}
	}

	/*
	 Restore vectors
	 */
	ierr = DMDAVecRestoreArrayDOF(da, C, &concentrations);
	checkPetscError(ierr, "PetscSolver1DHandler::initializeConcentration: "
			"DMDAVecRestoreArrayDOF failed.");

	return;
}

void PetscSolver1DHandler::updateConcentration(TS &ts, Vec &localC, Vec &F,
		PetscReal ftime) {
	PetscErrorCode ierr;

	// Get the local data vector from PETSc
	DM da;
	ierr = TSGetDM(ts, &da);
	checkPetscError(ierr, "PetscSolver1DHandler::updateConcentration: "
			"TSGetDM failed.");

	// Get the total size of the grid for the boundary conditions
	int xSize = grid.size();

	// Pointers to the PETSc arrays that start at the beginning (xs) of the
	// local array!
	PetscScalar **concs, **updatedConcs;
	// Get pointers to vector data
	ierr = DMDAVecGetArrayDOFRead(da, localC, &concs);
	checkPetscError(ierr, "PetscSolver1DHandler::updateConcentration: "
			"DMDAVecGetArrayDOFRead (localC) failed.");
	ierr = DMDAVecGetArrayDOF(da, F, &updatedConcs);
	checkPetscError(ierr, "PetscSolver1DHandler::updateConcentration: "
			"DMDAVecGetArrayDOF (F) failed.");

	// Get local grid boundaries
	PetscInt xs, xm;
	ierr = DMDAGetCorners(da, &xs, NULL, NULL, &xm, NULL, NULL);
	checkPetscError(ierr, "PetscSolver1DHandler::updateConcentration: "
			"DMDAGetCorners failed.");

	// The following pointers are set to the first position in the conc or
	// updatedConc arrays that correspond to the beginning of the data for the
	// current grid point. They are accessed just like regular arrays.
	PetscScalar *concOffset, *updatedConcOffset;

	// Get all the super clusters
	auto superClusters = network->getAll("Super");
	// Degrees of freedom is the total number of clusters in the network
	const int networkSize = network->size();
	const int dof = networkSize + 2 * superClusters.size();

	// Compute the total concentration of helium contained in HeV bubbles
	double heConc = 0.0;
	// Get all the HeV clusters in the network
	auto bubbles = network->getAll(xolotlCore::heVType);
	// Initialize for the loop
	xolotlCore::PSICluster * bubble;
	xolotlCore::SuperCluster * super;
	int index = 0;
	int heComp = 0;

	// Loop over grid points
	for (int xi = xs; xi < xs + xm; xi++) {
		// We are only interested in the helium near the surface
		if (grid[xi] - grid[surfacePosition] > 2.0) continue;

		// Get the concentrations at this grid point
		concOffset = concs[xi];
		// Copy data into the PSIClusterReactionNetwork
		network->updateConcentrationsFromArray(concOffset);

		// Loop on the bubbles
		for (int i = 0; i < bubbles.size(); i++) {
			// Get the bubble, its id, and its helium composition
			bubble = (xolotlCore::PSICluster *) bubbles.at(i);
			index = bubble->getId() - 1;
			auto comp = bubble->getComposition();
			heComp = comp[xolotlCore::heType];

			// Sum the helium concentration
			heConc += concOffset[index] * (double) heComp * (grid[xi] - grid[xi-1]);
		}
		// Loop on the super clusters
		for (int i = 0; i < superClusters.size(); i++) {
			super = (xolotlCore::SuperCluster *) superClusters[i];

			// Sum the helium concentration
			heConc += super->getTotalHeliumConcentration() * (grid[xi] - grid[xi-1]);
		}
	}

	// Share the concentration with all the processes
	double totalHeConc = 0.0;
	MPI_Allreduce(&heConc, &totalHeConc, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

	// Set the disappearing rate in the modified TM handler
	mutationHandler->updateDisappearingRate(totalHeConc);

	// Get the incident flux vector
	auto incidentFluxVector = fluxHandler->getIncidentFluxVec(ftime, surfacePosition);

	// Declarations for variables used in the loop
	double flux;
	int fluxIndex = fluxHandler->getIncidentFluxClusterIndex(), reactantIndex;
	xolotlCore::PSICluster *cluster = NULL;
	double **concVector = new double*[3];
	std::vector<double> gridPosition = { 0.0, 0.0, 0.0 };

	// Loop over grid points computing ODE terms for each grid point
	for (int xi = xs; xi < xs + xm; xi++) {
		// Compute the old and new array offsets
		concOffset = concs[xi];
		updatedConcOffset = updatedConcs[xi];

		// Boundary conditions
		// Everything to the left of the surface is empty
		if (xi <= surfacePosition || xi == xSize - 1) {
			for (int i = 0; i < dof; i++) {
				updatedConcOffset[i] = 1.0 * concOffset[i];
			}

			continue;
		}

		// Set the grid position
		gridPosition[0] = grid[xi];

		// Fill the concVector with the pointer to the middle, left, and right grid points
		concVector[0] = concOffset; // middle
		concVector[1] = concs[xi - 1]; // left
		concVector[2] = concs[xi + 1]; // right

		// Get the temperature from the temperature handler
		auto temperature = temperatureHandler->getTemperature(gridPosition,
				ftime);

		// Update the network if the temperature changed
		if (!xolotlCore::equal(temperature, lastTemperature)) {
			network->setTemperature(temperature);
			// Update the modified trap-mutation rate and the bubble bursting rate
			// that depends on the network reaction rates
			mutationHandler->updateTrapMutationRate(network);
			burstingHandler->updateBurstingRate(network);
			lastTemperature = temperature;
		}

		// Copy data into the PSIClusterReactionNetwork so that it can
		// compute the fluxes properly. The network is only used to compute the
		// fluxes and hold the state data from the last time step. I'm reusing
		// it because it cuts down on memory significantly (about 400MB per
		// grid point) at the expense of being a little tricky to comprehend.
		network->updateConcentrationsFromArray(concOffset);

		// ----- Account for flux of incoming He of cluster size 1 -----
		updatedConcOffset[fluxIndex] += incidentFluxVector[xi - surfacePosition];

		// ---- Compute diffusion over the locally owned part of the grid -----
		diffusionHandler->computeDiffusion(network, concVector,
				updatedConcOffset, grid[xi] - grid[xi-1], grid[xi+1] - grid[xi], xi);

		// ---- Compute advection over the locally owned part of the grid -----
		for (int i = 0; i < advectionHandlers.size(); i++) {
			advectionHandlers[i]->computeAdvection(network, gridPosition,
					concVector, updatedConcOffset, grid[xi] - grid[xi-1], grid[xi+1] - grid[xi], xi);
		}

		// ----- Compute the modified trap-mutation over the locally owned part of the grid -----
		mutationHandler->computeTrapMutation(network, xi, concOffset,
				updatedConcOffset);

		// ----- Compute the bubble bursting over the locally owned part of the grid -----
		burstingHandler->computeBursting(network, xi, updatedConcOffset);

		// ----- Compute all of the new fluxes -----
		for (int i = 0; i < networkSize; i++) {
			cluster = (xolotlCore::PSICluster *) allReactants->at(i);
			// Compute the flux
			flux = cluster->getTotalFlux();
			// Update the concentration of the cluster
			reactantIndex = cluster->getId() - 1;
			updatedConcOffset[reactantIndex] += flux;
		}

		// ---- Moments ----
		for (int i = 0; i < superClusters.size(); i++) {
			cluster = (xolotlCore::PSICluster *) superClusters[i];

			// Compute the helium momentum flux
			flux = cluster->getHeMomentFlux();
			// Update the concentration of the cluster
			reactantIndex = cluster->getHeMomentumId() - 1;
			updatedConcOffset[reactantIndex] += flux;

			// Compute the vacancy momentum flux
			flux = cluster->getVMomentFlux();
			// Update the concentration of the cluster
			reactantIndex = cluster->getVMomentumId() - 1;
			updatedConcOffset[reactantIndex] += flux;
		}
	}

	/*
	 Restore vectors
	 */
	ierr = DMDAVecRestoreArrayDOFRead(da, localC, &concs);
	checkPetscError(ierr, "PetscSolver1DHandler::updateConcentration: "
			"DMDAVecRestoreArrayDOFRead (localC) failed.");
	ierr = DMDAVecRestoreArrayDOF(da, F, &updatedConcs);
	checkPetscError(ierr, "PetscSolver1DHandler::updateConcentration: "
			"DMDAVecRestoreArrayDOF (F) failed.");
	ierr = DMRestoreLocalVector(da, &localC);
	checkPetscError(ierr, "PetscSolver1DHandler::updateConcentration: "
			"DMRestoreLocalVector failed.");

	// Clear the memory
	delete [] concVector;

	return;
}

void PetscSolver1DHandler::computeOffDiagonalJacobian(TS &ts, Vec &localC, Mat &J) const {
	PetscErrorCode ierr;

	// Get the distributed array
	DM da;
	ierr = TSGetDM(ts, &da);
	checkPetscError(ierr, "PetscSolver1DHandler::computeOffDiagonalJacobian: "
			"TSGetDM failed.");

	// Get the total size of the grid for the boundary conditions
	int xSize = grid.size();

	// Get local grid boundaries
	PetscInt xs, xm;
	ierr = DMDAGetCorners(da, &xs, NULL, NULL, &xm, NULL, NULL);
	checkPetscError(ierr, "PetscSolver1DHandler::computeOffDiagonalJacobian: "
			"DMDAGetCorners failed.");

	// Get the total number of diffusing clusters
	const int nDiff = diffusionHandler->getNumberOfDiffusing();

	// Get the total number of advecting clusters
	int nAdvec = 0;
	for (int l = 0; l < advectionHandlers.size(); l++) {
		int n = advectionHandlers[l]->getNumberOfAdvecting();
		if (n > nAdvec) nAdvec = n;
	}

	// Arguments for MatSetValuesStencil called below
	MatStencil row, cols[3];
	PetscScalar diffVals[3 * nDiff];
	PetscInt diffIndices[nDiff];
	PetscScalar advecVals[2 * nAdvec];
	PetscInt advecIndices[nAdvec];
	std::vector<double> gridPosition = { 0.0, 0.0, 0.0 };

	/*
	 Loop over grid points computing Jacobian terms for diffusion and advection
	 at each grid point
	 */
	for (int xi = xs; xi < xs + xm; xi++) {
		// Boundary conditions
		// Everything to the left of the surface is empty
		if (xi <= surfacePosition || xi == xSize - 1) continue;

		// Set the grid position
		gridPosition[0] = grid[xi];

		// Get the partial derivatives for the diffusion
		diffusionHandler->computePartialsForDiffusion(network, diffVals, diffIndices,
				grid[xi] - grid[xi-1], grid[xi+1] - grid[xi], xi);

		// Loop on the number of diffusion cluster to set the values in the Jacobian
		for (int i = 0; i < nDiff; i++) {
			// Set grid coordinate and component number for the row
			row.i = xi;
			row.c = diffIndices[i];

			// Set grid coordinates and component numbers for the columns
			// corresponding to the middle, left, and right grid points
			cols[0].i = xi; // middle
			cols[0].c = diffIndices[i];
			cols[1].i = xi - 1; // left
			cols[1].c = diffIndices[i];
			cols[2].i = xi + 1; // right
			cols[2].c = diffIndices[i];

			ierr = MatSetValuesStencil(J, 1, &row, 3, cols, diffVals + (3 * i), ADD_VALUES);
			checkPetscError(ierr, "PetscSolver1DHandler::computeOffDiagonalJacobian: "
					"MatSetValuesStencil (diffusion) failed.");
		}

		// Get the partial derivatives for the advection
		for (int l = 0; l < advectionHandlers.size(); l++) {
			advectionHandlers[l]->computePartialsForAdvection(network, advecVals,
					advecIndices, gridPosition, grid[xi] - grid[xi-1], grid[xi+1] - grid[xi], xi);

			// Get the stencil indices to know where to put the partial derivatives in the Jacobian
			auto advecStencil = advectionHandlers[l]->getStencilForAdvection(gridPosition);

			// Get the number of advecting clusters
			nAdvec = advectionHandlers[l]->getNumberOfAdvecting();

			// Loop on the number of advecting cluster to set the values in the Jacobian
			for (int i = 0; i < nAdvec; i++) {
				// Set grid coordinate and component number for the row
				row.i = xi;
				row.c = advecIndices[i];

				// If we are on the sink, the partial derivatives are not the same
				// Both sides are giving their concentrations to the center
				if (advectionHandlers[l]->isPointOnSink(gridPosition)) {
					cols[0].i = xi - advecStencil[0]; // left?
					cols[0].c = advecIndices[i];
					cols[1].i = xi + advecStencil[0]; // right?
					cols[1].c = advecIndices[i];
				}
				else {
					// Set grid coordinates and component numbers for the columns
					// corresponding to the middle and other grid points
					cols[0].i = xi; // middle
					cols[0].c = advecIndices[i];
					cols[1].i = xi + advecStencil[0]; // left or right
					cols[1].c = advecIndices[i];
				}

				// Update the matrix
				ierr = MatSetValuesStencil(J, 1, &row, 2, cols, advecVals + (2 * i), ADD_VALUES);
				checkPetscError(ierr, "PetscSolver1DHandler::computeOffDiagonalJacobian: "
						"MatSetValuesStencil (advection) failed.");
			}
		}
	}

	return;
}

void PetscSolver1DHandler::computeDiagonalJacobian(TS &ts, Vec &localC, Mat &J) {
	PetscErrorCode ierr;

	// Get the distributed array
	DM da;
	ierr = TSGetDM(ts, &da);
	checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: "
			"TSGetDM failed.");

	// Get the total size of the grid for the boundary conditions
	int xSize = grid.size();

	// Get pointers to vector data
	PetscScalar **concs;
	ierr = DMDAVecGetArrayDOFRead(da, localC, &concs);
	checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: "
			"DMDAVecGetArrayDOFRead failed.");

	// Get local grid boundaries
	PetscInt xs, xm;
	ierr = DMDAGetCorners(da, &xs, NULL, NULL, &xm, NULL, NULL);
	checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: "
			"DMDAGetCorners failed.");

	// Get all the super clusters
	auto superClusters = network->getAll("Super");
	// Degrees of freedom is the total number of clusters in the network
	const int networkSize = network->size();
	const int dof = networkSize + 2 * superClusters.size();

	// Pointer to the concentrations at a given grid point
	PetscScalar *concOffset;

	// Compute the total concentration of helium contained in HeV bubbles
	double heConc = 0.0;
	// Get all the HeV clusters in the network
	auto bubbles = network->getAll(xolotlCore::heVType);
	// Initialize for the loop
	xolotlCore::PSICluster * bubble;
	xolotlCore::SuperCluster * super;
	int index = 0;
	int heComp = 0;

	// Loop over grid points
	for (int xi = xs; xi < xs + xm; xi++) {
		// We are only interested in the helium near the surface
		if (grid[xi] - grid[surfacePosition] > 2.0) continue;

		// Get the concentrations at this grid point
		concOffset = concs[xi];
		// Copy data into the PSIClusterReactionNetwork
		network->updateConcentrationsFromArray(concOffset);

		// Loop on the bubbles
		for (int i = 0; i < bubbles.size(); i++) {
			// Get the bubble, its id, and its helium composition
			bubble = (xolotlCore::PSICluster *) bubbles.at(i);
			index = bubble->getId() - 1;
			auto comp = bubble->getComposition();
			heComp = comp[xolotlCore::heType];

			// Sum the helium concentration
			heConc += concOffset[index] * (double) heComp * (grid[xi] - grid[xi-1]);
		}
		// Loop on the super clusters
		for (int i = 0; i < superClusters.size(); i++) {
			super = (xolotlCore::SuperCluster *) superClusters[i];

			// Sum the helium concentration
			heConc += super->getTotalHeliumConcentration() * (grid[xi] - grid[xi-1]);
		}
	}

	// Share the concentration with all the processes
	double totalHeConc = 0.0;
	MPI_Allreduce(&heConc, &totalHeConc, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

	// Set the disappearing rate in the modified TM handler
	mutationHandler->updateDisappearingRate(totalHeConc);

	// Arguments for MatSetValuesStencil called below
	MatStencil rowId;
	MatStencil colIds[dof];
	int pdColIdsVectorSize = 0;

	// Store the total number of He clusters in the network for the
	// modified trap-mutation
	int nHelium = network->getAll(xolotlCore::heType).size();

	// Declarations for variables used in the loop
	int reactantIndex;

	// Loop over the grid points
	for (int xi = xs; xi < xs + xm; xi++) {
		// Boundary conditions
		// Everything to the left of the surface is empty
		if (xi <= surfacePosition || xi == xSize - 1) continue;

		// Copy data into the PSIClusterReactionNetwork so that it can
		// compute the new concentrations.
		concOffset = concs[xi];
		network->updateConcentrationsFromArray(concOffset);

		// Update the column in the Jacobian that represents each normal reactant
		for (int i = 0; i < networkSize - superClusters.size(); i++) {
			auto reactant = allReactants->at(i);
			// Get the reactant index
			reactantIndex = reactant->getId() - 1;

			// Set grid coordinate and component number for the row
			rowId.i = xi;
			rowId.c = reactantIndex;

			// Get the partial derivatives
			reactant->getPartialDerivatives(clusterPartials);
			// Get the list of column ids from the map
			auto pdColIdsVector = dFillMap.at(reactantIndex);
			// Number of partial derivatives
			pdColIdsVectorSize = pdColIdsVector.size();

			// Loop over the list of column ids
			for (int j = 0; j < pdColIdsVectorSize; j++) {
				// Set grid coordinate and component number for a column in the list
				colIds[j].i = xi;
				colIds[j].c = pdColIdsVector[j];

				// Get the partial derivative from the array of all of the partials
				reactingPartialsForCluster[j] =
						clusterPartials[pdColIdsVector[j]];

				// Reset the cluster partial value to zero. This is much faster
				// than using memset.
				clusterPartials[pdColIdsVector[j]] = 0.0;
			}

			// Update the matrix
			ierr = MatSetValuesStencil(J, 1, &rowId, pdColIdsVectorSize,
					colIds, reactingPartialsForCluster.data(), ADD_VALUES);
			checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: MatSetValuesStencil failed.");
		}

		// Update the column in the Jacobian that represents the moment for the super clusters
		for (int i = 0; i < superClusters.size(); i++) {
			auto reactant = (xolotlCore::SuperCluster *) superClusters[i];

			// Get the super cluster index
			reactantIndex = reactant->getId() - 1;
			// Set grid coordinate and component number for the row
			rowId.i = xi;
			rowId.c = reactantIndex;

			// Get the partial derivatives
			reactant->getPartialDerivatives(clusterPartials);
			// Get the list of column ids from the map
			auto pdColIdsVector = dFillMap.at(reactantIndex);
			// Number of partial derivatives
			pdColIdsVectorSize = pdColIdsVector.size();

			// Loop over the list of column ids
			for (int j = 0; j < pdColIdsVectorSize; j++) {
				// Set grid coordinate and component number for a column in the list
				colIds[j].i = xi;
				colIds[j].c = pdColIdsVector[j];
				// Get the partial derivative from the array of all of the partials
				reactingPartialsForCluster[j] =
						clusterPartials[pdColIdsVector[j]];

				// Reset the cluster partial value to zero. This is much faster
				// than using memset.
				clusterPartials[pdColIdsVector[j]] = 0.0;
			}
			// Update the matrix
			ierr = MatSetValuesStencil(J, 1, &rowId, pdColIdsVectorSize,
					colIds, reactingPartialsForCluster.data(), ADD_VALUES);
			checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: MatSetValuesStencil for super cluster failed.");

			// Get the helium momentum index
			reactantIndex = reactant->getHeMomentumId() - 1;
			// Set component number for the row
			rowId.c = reactantIndex;

			// Get the partial derivatives
			reactant->getHeMomentPartialDerivatives(clusterPartials);
			// Get the list of column ids from the map
			pdColIdsVector = dFillMap.at(reactantIndex);
			// Number of partial derivatives
			pdColIdsVectorSize = pdColIdsVector.size();

			// Loop over the list of column ids
			for (int j = 0; j < pdColIdsVectorSize; j++) {
				// Set grid coordinate and component number for a column in the list
				colIds[j].i = xi;
				colIds[j].c = pdColIdsVector[j];
				// Get the partial derivative from the array of all of the partials
				reactingPartialsForCluster[j] =
						clusterPartials[pdColIdsVector[j]];

				// Reset the cluster partial value to zero. This is much faster
				// than using memset.
				clusterPartials[pdColIdsVector[j]] = 0.0;
			}
			// Update the matrix
			ierr = MatSetValuesStencil(J, 1, &rowId, pdColIdsVectorSize,
					colIds, reactingPartialsForCluster.data(), ADD_VALUES);
			checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: MatSetValuesStencil for helium momentum failed.");

			// Get the vacancy momentum index
			reactantIndex = reactant->getVMomentumId() - 1;
			// Set component number for the row
			rowId.c = reactantIndex;

			// Get the partial derivatives
			reactant->getVMomentPartialDerivatives(clusterPartials);
			// Get the list of column ids from the map
			pdColIdsVector = dFillMap.at(reactantIndex);
			// Number of partial derivatives
			pdColIdsVectorSize = pdColIdsVector.size();

			// Loop over the list of column ids
			for (int j = 0; j < pdColIdsVectorSize; j++) {
				// Set grid coordinate and component number for a column in the list
				colIds[j].i = xi;
				colIds[j].c = pdColIdsVector[j];
				// Get the partial derivative from the array of all of the partials
				reactingPartialsForCluster[j] =
						clusterPartials[pdColIdsVector[j]];

				// Reset the cluster partial value to zero. This is much faster
				// than using memset.
				clusterPartials[pdColIdsVector[j]] = 0.0;
			}
			// Update the matrix
			ierr = MatSetValuesStencil(J, 1, &rowId, pdColIdsVectorSize,
					colIds, reactingPartialsForCluster.data(), ADD_VALUES);
			checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: MatSetValuesStencil for vacancy momentum failed.");
		}

		// ----- Take care of the bubble bursting for all the bubbles -----

		// Store the total number of HeV bubbles in the network for the
		// bubble bursting
		int nBubble = burstingHandler->getNBursting(network, xi);

		// Arguments for MatSetValuesStencil called below
		MatStencil row, col;
		PetscScalar burstingVals[nBubble];
		PetscInt burstingIndices[2 * nBubble];

		// Compute the partial derivative from bubble bursting at this grid point
		int nBursting = burstingHandler->computePartialsForBursting(network,
				burstingVals, burstingIndices, xi);

		// Loop on the number of bubbles bursting to set the values
		// in the Jacobian
		for (int i = 0; i < nBursting; i++) {
			// Set grid coordinate and component number for the row and column
			// corresponding to the bursting cluster
			row.i = xi;
			row.c = burstingIndices[2 * i];
			col.i = xi;
			col.c = burstingIndices[(2 * i) + 1];

			ierr = MatSetValuesStencil(J, 1, &row, 1, &col,
					burstingVals + i, ADD_VALUES);
			checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: "
					"MatSetValuesStencil (bursting) failed.");
		}

		// ----- Take care of the modified trap-mutation for all the reactants -----

		// Arguments for MatSetValuesStencil called below
		PetscScalar mutationVals[3 * nHelium];
		PetscInt mutationIndices[3 * nHelium];

		// Compute the partial derivative from modified trap-mutation at this grid point
		int nMutating = mutationHandler->computePartialsForTrapMutation(network,
				mutationVals, mutationIndices, xi);

		// Loop on the number of helium undergoing trap-mutation to set the values
		// in the Jacobian
		for (int i = 0; i < nMutating; i++) {
			// Set grid coordinate and component number for the row and column
			// corresponding to the helium cluster
			row.i = xi;
			row.c = mutationIndices[3 * i];
			col.i = xi;
			col.c = mutationIndices[3 * i];

			ierr = MatSetValuesStencil(J, 1, &row, 1, &col,
					mutationVals + (3 * i), ADD_VALUES);
			checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: "
					"MatSetValuesStencil (He trap-mutation) failed.");

			// Set component number for the row
			// corresponding to the HeV cluster created through trap-mutation
			row.c = mutationIndices[(3 * i) + 1];

			ierr = MatSetValuesStencil(J, 1, &row, 1, &col,
					mutationVals + (3 * i) + 1, ADD_VALUES);
			checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: "
					"MatSetValuesStencil (HeV trap-mutation) failed.");

			// Set component number for the row
			// corresponding to the interstitial created through trap-mutation
			row.c = mutationIndices[(3 * i) + 2];

			ierr = MatSetValuesStencil(J, 1, &row, 1, &col,
					mutationVals + (3 * i) + 2, ADD_VALUES);
			checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: "
					"MatSetValuesStencil (I trap-mutation) failed.");
		}
	}

	/*
	 Restore vectors
	 */
	ierr = DMDAVecRestoreArrayDOFRead(da, localC, &concs);
	checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: "
			"DMDAVecRestoreArrayDOFRead failed.");
	ierr = DMRestoreLocalVector(da, &localC);
	checkPetscError(ierr, "PetscSolver1DHandler::computeDiagonalJacobian: "
			"DMRestoreLocalVector failed.");

	return;
}

} /* end namespace xolotlSolver */
