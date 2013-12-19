/*
 * PSIClusterTester.cpp
 *
 *  Created on: May 6, 2013
 *      Author: Jay Jay Billings
 */
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Regression

#include <boost/test/included/unit_test.hpp>
#include <PSICluster.h>
#include <VCluster.h>
#include "SimpleReactionNetwork.h"
#include <memory>
#include <typeinfo>
#include <limits>
#include <algorithm>

using namespace std;
using namespace xolotlCore;
using namespace testUtils;

/**
 * This suite is responsible for testing the VCluster.
 */BOOST_AUTO_TEST_SUITE(VCluster_testSuite)

/**
 * This operation checks the ability of the VCluster to describe
 * its connectivity to other clusters.
 */
BOOST_AUTO_TEST_CASE(checkConnectivity) {

	shared_ptr<ReactionNetwork> network = getSimpleReactionNetwork();
	auto props = network->getProperties();

	// Prevent dissociation from being added to the connectivity array
	props["dissociationsEnabled"] = "false";

	// Check the connectivity of the 2nd V reactant (numV=2)

	{
		// Get the connectivity array from the reactant for a vacancy cluster of size 2.
		auto reactant = std::dynamic_pointer_cast < PSICluster
				> (network->get("V", 2));
		auto reactionConnectivity = reactant->getConnectivity();

		// Check the connectivity for He, V, and I

		int connectivityExpected[] = {
				// He
				1, 1, 1, 1, 1, 1, 1, 1, 0, 0,

				// V
				1, 1, 1, 1, 1, 1, 1, 1, 0, 0,

				// I
				1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

				// HeV
				// The VCluster type only reacts with HeV for
				// single-V clusters.
				0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
				0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
				0, 0, 0,

				// HeI
				0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
				1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
				1, 1, 1 };

		for (int i = 0; i < reactionConnectivity.size(); i++) {
			BOOST_REQUIRE_EQUAL(reactionConnectivity[i],
					connectivityExpected[i]);
		}
	}

	return;
}

/**
 * This operation checks the reaction radius for InterstitialCluster.
 */BOOST_AUTO_TEST_CASE(checkReactionRadius) {

	std::vector<std::shared_ptr<VCluster>> clusters;
	std::shared_ptr<VCluster> cluster;
	double expectedRadii[] = { 0.0 };

	for (int i = 1; i <= 10; i++) {
		cluster = std::shared_ptr < VCluster > (new VCluster(i));
		//BOOST_CHECK_CLOSE(expectedRadii[i - 1], cluster->getReactionRadius(),
		//	.000001);
	}
}
BOOST_AUTO_TEST_SUITE_END()
