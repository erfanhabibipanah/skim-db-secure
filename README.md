# SKiM-db

This repository contains a C++ implementation of the database components for [SKiM](https://gitlab.com/SCoRe-Group/SKiM) (Short K-mers in Metagenomics), a memory-efficient metagenomic classifier for Oxford Nanopore Technologies (ONT) reads.

## About SKiM

SKiM is originally a Rust-based tool designed for DNA classification in metagenomic datasets. It uses short k-mers (typically k=15 or k=16) with advanced data compression and statistical correction techniques to achieve fast, accurate classification while maintaining low memory consumption.

## Purpose

This repository provides C++ database implementation of the SKiM classification pipeline.

## Related Projects

- [SKiM main repository](https://gitlab.com/SCoRe-Group/SKiM) - The primary Rust implementation of the SKiM classifier
