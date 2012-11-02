#include "mpc/module/PhotoDisintegration.h"

#include <cmath>
#include <limits>
#include <sstream>
#include <fstream>
#include <stdexcept>

namespace mpc {

PhotoDisintegration::PhotoDisintegration(PhotonField photonField) {
	init(photonField);
}

void PhotoDisintegration::init(PhotonField photonField) {
	this->photonField = photonField;
	switch (photonField) {
	case CMB:
		setDescription("PhotoDisintegration: CMB");
		init(getDataPath("photodis_CMB.txt"));
		break;
	case IRB:
		setDescription("PhotoDisintegration: IRB");
		init(getDataPath("photodis_IRB.txt"));
		break;
	case CMB_IRB:
		setDescription("PhotoDisintegration: CMB and IRB");
		init(getDataPath("photodis_CMB_IRB.txt"));
		break;
	default:
		throw std::runtime_error(
				"mpc::PhotoDisintegration: unknown photon background");
	}
}

void PhotoDisintegration::init(std::string filename) {
	pdTable.resize(31 * 57);

	// create spline x-axis
	std::ifstream infile(filename.c_str());
	if (!infile.good())
		throw std::runtime_error(
				"mpc::PhotoDisintegration: could not open file " + filename);

	std::string line;
	while (std::getline(infile, line)) {
		if (line[0] == '#')
			continue;
		std::stringstream lineStream(line);

		int Z, N;
		lineStream >> Z; // charge number
		lineStream >> N; // mass number

		PDMode pd;
		lineStream >> pd.channel; // disintegration channel

		double r = 0;
		for (size_t i = 0; i < 200; i++) {
			lineStream >> r;
			pd.rate.push_back(r / Mpc); // disintegration rate in [1/m]
		}

		pdTable[Z * 31 + N].push_back(pd);
	}

	infile.close();
}

bool PhotoDisintegration::setNextInteraction(Candidate *candidate,
		InteractionState &interaction) const {
	int A = candidate->current.getMassNumber();
	int Z = candidate->current.getChargeNumber();
	int N = A - Z;

	// check if disintegration data available
	std::vector<PDMode> pdModes = pdTable[Z * 31 + N];
	if (pdModes.size() == 0)
		return false;

	// CMB energy increases with (1+z), increase nucleus energy accordingly
	double z = candidate->getRedshift();
	double lg = log10(candidate->current.getLorentzFactor() * (1 + z));

	// check if out of energy range
	if ((lg <= 6) or (lg >= 14))
		return false;

	// find channel with minimum random decay distance
	Random &random = Random::instance();
	interaction.distance = std::numeric_limits<double>::max();
	for (size_t i = 0; i < pdModes.size(); i++) {
		double rate = interpolateEquidistant(lg, 6, 14, pdModes[i].rate);
		double d = -log(random.rand()) / rate;
		if (d > interaction.distance)
			continue;
		interaction.distance = d;
		interaction.channel = pdModes[i].channel;
	}

	// interaction length is proportional to 1 / (photon density)
	interaction.distance /= photonFieldScaling(photonField, z);
	// convert to comoving frame
	interaction.distance *= (1 + z);

	candidate->setInteractionState(getDescription(), interaction);
	return true;
}

void PhotoDisintegration::performInteraction(Candidate *candidate) const {
	InteractionState interaction;
	candidate->getInteractionState(getDescription(), interaction);
	candidate->clearInteractionStates();

	// parse disintegration channel
	int nNeutron = digit(interaction.channel, 100000);
	int nProton = digit(interaction.channel, 10000);
	int nH2 = digit(interaction.channel, 1000);
	int nH3 = digit(interaction.channel, 100);
	int nHe3 = digit(interaction.channel, 10);
	int nHe4 = digit(interaction.channel, 1);

	int dA = -nNeutron - nProton - 2 * nH2 - 3 * nH3 - 3 * nHe3 - 4 * nHe4;
	int dZ = -nProton - nH2 - nH3 - 2 * nHe3 - 2 * nHe4;

	int A = candidate->current.getMassNumber();
	int Z = candidate->current.getChargeNumber();
	double EpA = candidate->current.getEnergy() / double(A);

	// update particle
	int nA = A + dA;
	if (nA > 0) {
		candidate->current.setId(getNucleusId(A + dA, Z + dZ));
		candidate->current.setEnergy(EpA * (A + dA));
	} else {
		candidate->setActive(false);
	}

	// create secondaries
	for (size_t i = 0; i < nNeutron; i++)
		candidate->addSecondary(getNucleusId(1, 0), EpA);
	for (size_t i = 0; i < nProton; i++)
		candidate->addSecondary(getNucleusId(1, 1), EpA);
	for (size_t i = 0; i < nH2; i++)
		candidate->addSecondary(getNucleusId(2, 1), EpA * 2);
	for (size_t i = 0; i < nH3; i++)
		candidate->addSecondary(getNucleusId(3, 1), EpA * 3);
	for (size_t i = 0; i < nHe3; i++)
		candidate->addSecondary(getNucleusId(3, 2), EpA * 3);
	for (size_t i = 0; i < nHe4; i++)
		candidate->addSecondary(getNucleusId(4, 2), EpA * 4);
}

double PhotoDisintegration::energyLossLength(int id, double E) {
	int A = getMassNumberFromNucleusId(id);
	int Z = getChargeNumberFromNucleusId(id);
	int N = A - Z;

	std::vector<PDMode> pdModes = pdTable[Z * 31 + N];
	if (pdModes.size() == 0)
		return std::numeric_limits<double>::max();

	// log10 of lorentz factor
	double lg = log10(E / (getNucleusMass(id) * c_squared));
	if ((lg <= 6) or (lg >= 14))
		return std::numeric_limits<double>::max();

	double lossRate = 0;
	for (size_t i = 0; i < pdModes.size(); i++) {
		double rate = interpolateEquidistant(lg, 6, 14, pdModes[i].rate);

		int channel = pdModes[i].channel;
		int nN = digit(channel, 100000);
		int nP = digit(channel, 10000);
		int nH2 = digit(channel, 1000);
		int nH3 = digit(channel, 100);
		int nHe3 = digit(channel, 10);
		int nHe4 = digit(channel, 1);

		double relativeEnergyLoss = double(
				nN + nP + 2 * nH2 + 3 * nH3 + 3 * nHe3 + 4 * nHe4) / double(A);

		lossRate += rate * relativeEnergyLoss;
	}

	return 1 / lossRate;
}

} // namespace mpc
