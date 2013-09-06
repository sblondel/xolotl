#ifndef HECLUSTER_H
#define HECLUSTER_H

// Includes
#include "PSICluster.h"

namespace xolotlCore {

/**
 * This class represents a cluster composed entirely of helium.
 */
class HeCluster: public PSICluster {

private:

	/**
	 * The default constructor is private because PSIClusters must always be
	 * initialized with a size.
	 */
	HeCluster():PSICluster(1) {}

public:

	/**
	 * The constructor. All HeClusters must be initialized with a size.
	 * @param nHe the number of helium atoms in the cluster
	 */
	HeCluster(int nHe);

	/**
	 * Destructor
	 */
	~HeCluster();

	/**
	 * This operation returns a Reactant that is created using the copy
	 * constructor of HeCluster.
	 * @return A copy of this HeCluster.
	 */
	virtual std::shared_ptr<Reactant> clone();

	/**
	 * This virtual method is for subclasses to specialize
	 * to return their representative cluster map, which is a mapping
	 * of which species exist in the cluster to the integer number
	 * of each species.
	 *
	 * @return
	 */
	virtual std::map<std::string, int> getClusterMap();

	/**
	 * Return whether or not this PSICluster is a product
	 * of the reaction between reactantI and reactantJ in
	 * this Reactants ReactionNetwork. This method should be
	 * specialized by subclasses to indicate whether or not they
	 * are the product of the given reaction.
	 *
	 * @param reactantI
	 * @param reactantJ
	 * @return true if this reactant is a product of i and j
	 */
	virtual bool isProductReactant(const Reactant & reactantI, const Reactant & reactantJ);

	/**
	 * This virtual method is for subclasses to specialize
	 * and should return the reaction radius for the
	 * particular PSICluster subclass.
	 *
	 * @return
	 */
	virtual double getReactionRadius();
	
	/**
	 * This operation returns the compositon of this reactant.
	 * @return The composition returned as a map with keys naming distinct
	 * elements and values indicating the amount of the element present.
	 */
	virtual std::map<std::string,int> getComposition() const;

protected:

	/**
	 * Computes a row of the reaction connectivity matrix corresponding to
	 * this reactant.
	 *
	 * If two reactants alone can form a reaction, the element at the position
	 * of the second reactant is 1, otherwise 0.
	 */
	void createReactionConnectivity();
	
	/**
	 * Computes a row of the dissociation connectivity matrix corresponding to
	 * this reactant.
	 *
	 * If two reactants together can be produced by a single reaction,
	 * the element at the position of the second reactant is 1, otherwise 0.
	 */
	void createDissociationConnectivity();

};

//end class HeCluster

} /* namespace xolotlCore */
#endif